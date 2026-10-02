/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <thread>
#ifdef __ANDROID__
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include "platform/android/media_frames.h"
#endif

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  namespace {
#ifdef __ANDROID__
    // Aggregate counters only: never retain or inspect private playback PCM.
    class PlaybackCounters {
    public:
      using clock = std::chrono::steady_clock;
      explicit PlaybackCounters(int session_id): session_id_(session_id) {}
      ~PlaybackCounters() { report(clock::now()); }
      void read_result(int count, size_t requested) {
        ++reads_;
        if (count == 0) ++zeros_;
        if (count > 0 && static_cast<size_t>(count) < requested) ++shorts_;
        const auto now = clock::now();
        if (now - window_ >= std::chrono::seconds(5)) report(now);
      }
      void admitted(size_t count, size_t dropped, size_t queued) {
        samples_ += count;
        drops_ += dropped;
        queue_max_ = std::max(queue_max_, queued);
      }
    private:
      void report(clock::time_point now) {
        if (!reads_) return;
        BOOST_LOG(info) << "Audio read counters session=" << session_id_
                        << " reads=" << reads_ << " zero=" << zeros_ << " short=" << shorts_ << " samples=" << samples_
                        << " dropped_packets=" << drops_ << " queue_max=" << queue_max_;
        window_ = now;
        reads_ = zeros_ = shorts_ = samples_ = drops_ = queue_max_ = 0;
      }
      int session_id_;
      clock::time_point window_ = clock::now();
      size_t reads_ = 0, zeros_ = 0, shorts_ = 0, samples_ = 0, drops_ = 0, queue_max_ = 0;
    };
#endif
    bool valid_duration(int duration) {
      return duration == 5 || duration == 10 || duration == 20 || duration == 40 || duration == 60;
    }

    template<class Next, class Fail>
    void encode_samples(const config_t &config, void *channel_data, Next next, Fail fail) {
      if (!valid_duration(config.packetDuration) ||
          (config.channels != 2 && config.channels != 6 && config.channels != 8)) {
        fail();
        return;
      }
      auto packets = mail::man->queue<packet_t>(mail::audio_packets);
      auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
      if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
        apply_surround_params(stream, config.customStreamParams);
      }
      if (stream.channelCount != config.channels) {
        fail();
        return;
      }
      platf::adjust_thread_priority(platf::thread_priority_e::high);
      int opus_error = OPUS_OK;
      opus_t opus {opus_multistream_encoder_create(stream.sampleRate, stream.channelCount,
        stream.streams, stream.coupledStreams, stream.mapping,
        OPUS_APPLICATION_RESTRICTED_LOWDELAY, &opus_error)};
      if (!opus || opus_error != OPUS_OK ||
          opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate)) != OPUS_OK ||
          opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0)) != OPUS_OK) {
        BOOST_LOG(error) << "Couldn't initialize Opus encoder"sv;
        fail();
        return;
      }
      const int frame_size = config.packetDuration * stream.sampleRate / 1000;
      while (auto sample = next()) {
        if (sample->size() != static_cast<size_t>(frame_size * stream.channelCount)) {
          BOOST_LOG(error) << "Invalid PCM packet size"sv;
          fail();
          return;
        }
        buffer_t packet {1400};
        const int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size,
                                                       std::begin(packet), packet.size());
        if (bytes <= 0) {
          BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
          fail();
          return;
        }
        packet.fake_resize(bytes);
        packets->raise(channel_data, std::move(packet));
      }
    }
  }

  static void encode_session(sample_queue_t samples, config_t config, void *channel_data, safe::mail_t session_mail) {
    encode_samples(config, channel_data, [&]() { return samples->pop(); }, [&]() {
      samples->stop();
      if (session_mail) {
        session_mail->event<bool>(mail::shutdown)->raise(true);
      }
    });
  }

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    encode_session(std::move(samples), config, channel_data, {});
  }

#ifdef __ANDROID__
  namespace {
    class JniAttachment {
    public:
      explicit JniAttachment(JavaVM *vm): vm_(vm) {
        if (!vm_) return;
        const auto status = vm_->GetEnv(reinterpret_cast<void **>(&env_), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
          attached_ = vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK;
          if (!attached_) env_ = nullptr;
        } else if (status != JNI_OK) {
          env_ = nullptr;
        }
      }
      ~JniAttachment() { if (attached_) vm_->DetachCurrentThread(); }
      JNIEnv *get() const { return env_; }
    private:
      JavaVM *vm_;
      JNIEnv *env_ = nullptr;
      bool attached_ = false;
    };

    class JniRef {
    public:
      JniRef(JNIEnv *env, jobject ref, bool global = false): env_(env), ref_(ref), global_(global) {}
      ~JniRef() {
        if (ref_) {
          if (global_) env_->DeleteGlobalRef(ref_);
          else env_->DeleteLocalRef(ref_);
        }
      }
      JniRef(const JniRef &) = delete;
      JniRef &operator=(const JniRef &) = delete;
      jobject get() const { return ref_; }
    private:
      JNIEnv *env_;
      jobject ref_;
      bool global_;
    };

    bool jni_failed(JNIEnv *env) {
      if (!env->ExceptionCheck()) return false;
      env->ExceptionDescribe();
      env->ExceptionClear();
      return true;
    }

    void set_audio_thread_priority(JNIEnv *env) {
      // Native workers otherwise inherit ordinary-app nice 0: the platform
      // high-priority hook is a no-op on Android. Use the public Android API
      // for these two deadline-sensitive workers, not the whole host process.
      JniRef process(env, env->FindClass("android/os/Process"));
      if (jni_failed(env) || !process.get()) return;
      auto set_priority = env->GetStaticMethodID(static_cast<jclass>(process.get()), "setThreadPriority", "(I)V");
      if (jni_failed(env) || !set_priority) return;
      env->CallStaticVoidMethod(static_cast<jclass>(process.get()), set_priority, -16); // THREAD_PRIORITY_AUDIO
      if (jni_failed(env)) BOOST_LOG(warning) << "Android audio thread priority unavailable"sv;
    }
  }

  void capture_android(void *channel_data, safe::mail_t session_mail, const config_t &config,
                       JavaVM *vm, jclass bridge, int session_id) {
    auto shutdown = session_mail->event<bool>(mail::shutdown);
    const auto fail = [&]() {
      BOOST_LOG(error) << "Android playback capture failed"sv;
      shutdown->raise(true);
    };
    if (config.channels != 2 || !valid_duration(config.packetDuration) ||
        config.flags[config_t::CUSTOM_SURROUND_PARAMS] || !vm || !bridge) {
      fail();
      return;
    }
    try {
      // Construct the bounded session queue before Java starts AudioRecord.
      const size_t packet_samples = static_cast<size_t>(config.packetDuration) * 48 * 2;
      android_media::PcmPacketizer pcm(packet_samples);
      std::mutex mutex;
      std::condition_variable ready;
      bool stopped = false;
      JniAttachment attachment(vm);
      auto *env = attachment.get();
      if (!env) { fail(); return; }
      JniRef bridge_ref(env, env->NewGlobalRef(bridge), true);
      if (jni_failed(env) || !bridge_ref.get()) { fail(); return; }
      auto bridge_class = static_cast<jclass>(bridge_ref.get());
      auto start = env->GetStaticMethodID(bridge_class, "startPlaybackCapture", "(II)Landroid/media/AudioRecord;");
      if (jni_failed(env) || !start) { fail(); return; }
      auto stop = env->GetStaticMethodID(bridge_class, "stopPlaybackCapture", "(I)V");
      if (jni_failed(env) || !stop) { fail(); return; }
      auto release = env->GetStaticMethodID(bridge_class, "releasePlaybackCapture", "(I)V");
      if (jni_failed(env) || !release) { fail(); return; }
      if (shutdown->peek()) return;
      // This guard also handles partial Java initialization failures.
      auto java_cleanup = util::fail_guard([&]() {
        env->CallStaticVoidMethod(bridge_class, stop, session_id);
        if (jni_failed(env)) fail();
        env->CallStaticVoidMethod(bridge_class, release, session_id);
        if (jni_failed(env)) fail();
      });
      JniRef record_local(env, env->CallStaticObjectMethod(bridge_class, start, session_id, config.packetDuration));
      if (jni_failed(env)) { fail(); return; }
      if (!record_local.get()) {
        BOOST_LOG(info) << "Playback capture unavailable; continuing video-only"sv;
        // Keep the audio owner alive until session shutdown, so its caller's
        // thread-exit guard does not terminate otherwise valid video capture.
        shutdown->view();
        return;
      }
      JniRef record(env, env->NewGlobalRef(record_local.get()), true);
      if (jni_failed(env) || !record.get()) { fail(); return; }
      JniRef record_class(env, env->GetObjectClass(record.get()));
      if (jni_failed(env) || !record_class.get()) { fail(); return; }
      auto read = env->GetMethodID(static_cast<jclass>(record_class.get()), "read", "([FIII)I");
      if (jni_failed(env) || !read) { fail(); return; }

      std::thread reader;
      std::thread encoder;
      auto workers_cleanup = util::fail_guard([&]() {
        // stop unblocks Java first; release must never race an active reader.
        env->CallStaticVoidMethod(bridge_class, stop, session_id);
        if (jni_failed(env)) fail();
        {
          std::lock_guard lock(mutex);
          stopped = true;
        }
        ready.notify_all();
        if (reader.joinable()) reader.join();
        if (encoder.joinable()) encoder.join();
        env->CallStaticVoidMethod(bridge_class, release, session_id);
        if (jni_failed(env)) fail();
        java_cleanup.disable();
      });
      encoder = std::thread([&]() {
        try {
          JniAttachment encoder_attachment(vm);
          if (auto *encoder_env = encoder_attachment.get()) set_audio_thread_priority(encoder_env);
          encode_samples(config, channel_data, [&]() -> std::optional<std::vector<float>> {
            std::unique_lock lock(mutex);
            while (!stopped && !shutdown->peek()) {
              if (auto packet = pcm.pop()) {
                lock.unlock();
                ready.notify_one();
                return packet;
              }
              ready.wait_for(lock, 5ms);
            }
            return std::nullopt;
          }, fail);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Android audio encoder: " << e.what();
          fail();
        }
      });
      reader = std::thread([&]() {
        try {
          JniAttachment reader_attachment(vm);
          auto *reader_env = reader_attachment.get();
          if (!reader_env) { fail(); return; }
          set_audio_thread_priority(reader_env);
          JniRef array(reader_env, reader_env->NewFloatArray(static_cast<jsize>(packet_samples)));
          if (jni_failed(reader_env) || !array.get()) { fail(); return; }
          std::vector<float> buffer(packet_samples);
          PlaybackCounters counters(session_id);
          while (!shutdown->peek()) {
            {
              std::unique_lock lock(mutex);
              // AudioRecord exposes PCM in bursts, not one negotiated packet
              // every 5 ms. Leave unread data in its bounded capture buffer
              // until the encoder has a complete-packet credit. Greedily
              // draining a fifth packet here used to discard valid PCM before
              // the just-notified encoder could run.
              if (!pcm.can_accept_packet() && !stopped && !shutdown->peek()) {
                ready.wait_for(lock, std::chrono::milliseconds(config.packetDuration), [&]() {
                  return stopped || shutdown->peek() || pcm.can_accept_packet();
                });
              }
              if (stopped || shutdown->peek()) break;
              if (!pcm.can_accept_packet()) continue;
            }
            // READ_NON_BLOCKING=1 keeps cancellation finite even with no PCM.
            const jint count = reader_env->CallIntMethod(record.get(), read, array.get(),
                                                         0, static_cast<jint>(packet_samples), 1);
            counters.read_result(count, packet_samples);
            if (jni_failed(reader_env) || count < 0 || static_cast<size_t>(count) > packet_samples) {
              if (!shutdown->peek()) fail();
              return;
            }
            if (count > 0) {
              reader_env->GetFloatArrayRegion(static_cast<jfloatArray>(array.get()), 0, count, buffer.data());
              if (jni_failed(reader_env)) { fail(); return; }
              size_t dropped = 0, queued = 0;
              {
                std::lock_guard lock(mutex);
                if (stopped) break;
                const auto before = pcm.queued_packets();
                const auto produced = (pcm.pending_samples() + static_cast<size_t>(count)) / packet_samples;
                pcm.push(buffer.data(), static_cast<size_t>(count));
                queued = pcm.queued_packets();
                dropped = before + produced - queued;
              }
              counters.admitted(static_cast<size_t>(count), dropped, queued);
              ready.notify_one();
            }
            if (static_cast<size_t>(count) < packet_samples) std::this_thread::sleep_for(2ms);
          }
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Android audio reader: " << e.what();
          fail();
        }
      });
      shutdown->view();
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Android audio session: " << e.what();
      fail();
    }
  }
#endif

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encode_session, samples, config, channel_data, mail};

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    return 0;
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }
}  // namespace audio
