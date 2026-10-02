#include <jni.h>
#include <string>
#include "logging.h"
#include "config.h"
#include "nvhttp.h"
#include "globals.h"
#include "sunshine.h"
#include "stream.h"
#include "rtsp.h"
#include "audio.h"
#include "video_colorspace.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <boost/endian/buffers.hpp>
#include <mutex>
#include "platform/android/input_bridge.h"
#include "input.h"
#include "platform/android/media_frames.h"
#include <ctime>
#include <atomic>
#include <cmath>
#include <climits>

using namespace std::literals;

extern "C" {

static std::unique_ptr<logging::deinit_t> deinit;
static JavaVM *jvm = nullptr;
static std::mutex host_mutex;
static bool host_active = false;
static bool host_stop_requested = false;
static unsigned ready_listeners = 0;
static bool startup_failed = false;
static jclass sunshineServerClass = nullptr;

/// Create a Java Integer object
jobject createJavaInt(JNIEnv *env, int value) {
    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID integerConstructor = env->GetMethodID(integerClass, "<init>", "(I)V");
    return env->NewObject(integerClass, integerConstructor, value);
}
/// 一个封装好的函数，用于调用 Java 方法
/// A helper function to call Java methods
void invokeJavaFunction(
        const char *name,
        const char *sig
        ...
) {
    if (jvm == nullptr) {
        BOOST_LOG(error) << "JVM is null!"sv;
        return;
    }
    JNIEnv *env = nullptr;
    bool attached = jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    if (!env) return;

    jmethodID method = env->GetStaticMethodID(sunshineServerClass, name, sig);
    if (method == nullptr) {
        BOOST_LOG(error) << "Cannot find method "sv << name << " with signature "sv << sig;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (attached) jvm->DetachCurrentThread();
        return;
    }
    va_list args;
    va_start(args, sig);
    env->CallStaticVoidMethodV(sunshineServerClass, method, args);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) jvm->DetachCurrentThread();
}
/// Create a Java Double object
jobject createJavaDouble(JNIEnv *env, double value) {
    jclass doubleClass = env->FindClass("java/lang/Double");
    jmethodID doubleConstructor = env->GetMethodID(doubleClass, "<init>", "(D)V");
    return env->NewObject(doubleClass, doubleConstructor, value);
}

jobject createHashMap(JNIEnv *env) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID hashMapConstructor = env->GetMethodID(hashMapClass, "<init>", "()V");
    return env->NewObject(hashMapClass, hashMapConstructor);
}

/// 将 C 中 的 int、double、std::string 转换为 Java 中的 Integer、Double、String
/// Convert int, double, and std::string in C to Integer, Double, and String in Java
jobject convertToJavaObject(JNIEnv *env, const std::any &value) {
    if (value.type() == typeid(int)) {
        return createJavaInt(env, std::any_cast<int>(value));
    } else if (value.type() == typeid(double)) {
        return createJavaDouble(env, std::any_cast<double>(value));
    } else {
        return env->NewStringUTF(std::any_cast<std::string>(value).c_str());
    }
}
void putValueInJavaHashMap(
        JNIEnv *env,
        jobject hashMap,
        const std::string &key,
        const std::any &value
) {
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    const char *sig = "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;";
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", sig);

    jstring javaKey = env->NewStringUTF(key.c_str());
    jobject javaValue = convertToJavaObject(env, value);

    env->CallObjectMethod(hashMap, putMethod, javaKey, javaValue);

    env->DeleteLocalRef(javaKey);
    env->DeleteLocalRef(javaValue);
}

jobject convertMapToJavaHashMap(JNIEnv *env, const std::map<std::string, std::any> &cppMap) {
    jobject map = createHashMap(env);

    // Iterate over the C++ map and put entries into the HashMap
    for (const auto &entry: cppMap) {
        putValueInJavaHashMap(env, map, entry.first, entry.second);
    }

    return map;
}


JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_start(JNIEnv *env, jclass clazz) {
    {
        std::lock_guard lock(host_mutex);
        if (host_stop_requested) return;
        if (host_active) {
            jclass error = env->FindClass("java/lang/IllegalStateException");
            env->ThrowNew(error, "Sunshine host already started");
            return;
        }
        host_active = true;
        ready_listeners = 0;
        startup_failed = false;
        env->GetJavaVM(&jvm);
        sunshineServerClass = static_cast<jclass>(env->NewGlobalRef(clazz));
        mail::man = std::make_shared<safe::mail_raw_t>();
    }
    std::thread httpThread;
    bool pool_started = false;
    std::unique_ptr<platf::deinit_t> input_owner;
    try {
        if (!sunshineServerClass) throw std::runtime_error("Native bridge unavailable");
        deinit = logging::init(1, "/dev/null");
        if (!platf::android_input::initialize(env)) throw std::runtime_error("Input bridge unavailable");
        input_owner = input::init();
        sunshine_callbacks::initializeVideoCapabilities();
        task_pool.start(1);
        pool_started = true;
        httpThread = std::thread([] {
            try { nvhttp::start(); }
            catch (const std::exception &error) { sunshine_callbacks::hostStartupFailed(error.what()); }
            catch (...) { sunshine_callbacks::hostStartupFailed("HTTP startup failed"); }
        });
        rtsp_stream::rtpThread();
    } catch (const std::exception &error) {
        sunshine_callbacks::hostStartupFailed(error.what());
    } catch (...) {
        sunshine_callbacks::hostStartupFailed("Native startup failed");
    }
    mail::man->event<bool>(mail::shutdown)->raise(true);
    if (httpThread.joinable()) httpThread.join();
    if (pool_started) { task_pool.stop(); task_pool.join(); }
    input_owner.reset();
    {
        std::lock_guard lock(host_mutex);
        if (sunshineServerClass) env->DeleteGlobalRef(sunshineServerClass);
        sunshineServerClass = nullptr;
        host_active = false;
    }
    deinit.reset();
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_prepareStart(JNIEnv *, jclass) {
    std::lock_guard lock(host_mutex);
    if (!host_active) host_stop_requested = false;
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_stop(JNIEnv *, jclass) {
    std::lock_guard lock(host_mutex);
    host_stop_requested = true;
    if (host_active && mail::man) mail::man->event<bool>(mail::shutdown)->raise(true);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setSunshineName(JNIEnv *env, jclass clazz,
                                                         jstring sunshine_name) {
    const char *str = env->GetStringUTFChars(sunshine_name, nullptr);
    config::nvhttp.sunshine_name = str;
    env->ReleaseStringUTFChars(sunshine_name, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setPkeyPath(JNIEnv *env, jclass clazz, jstring path) {
    const char *str = env->GetStringUTFChars(path, nullptr);
    config::nvhttp.pkey = str;
    env->ReleaseStringUTFChars(path, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setCertPath(JNIEnv *env, jclass clazz, jstring path) {
    const char *str = env->GetStringUTFChars(path, nullptr);
    config::nvhttp.cert = str;
    env->ReleaseStringUTFChars(path, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_setFileStatePath(JNIEnv *env, jclass clazz,
                                                          jstring path) {
    const char *str = env->GetStringUTFChars(path, nullptr);
    config::nvhttp.file_state = str;
    env->ReleaseStringUTFChars(path, str);
}

JNIEXPORT void JNICALL
Java_com_nightmare_sunshine_NativeBridge_submitPin(JNIEnv *env, jclass clazz, jstring pin) {
    const char *pinStr = env->GetStringUTFChars(pin, nullptr);
    nvhttp::pin(pinStr, "some-moonlight");
    env->ReleaseStringUTFChars(pin, pinStr);
}

// Native bridge class is released only after all owned host workers have joined.


}

AMediaFormat *createFormat(const video::config_t &config) {
    auto *format = AMediaFormat_new();
    if (!format) throw std::runtime_error("Media format allocation failed");
    AMediaFormat_setString(format, "mime", config.videoFormat == 1 ? "video/hevc" : "video/avc");
    AMediaFormat_setInt32(format, "width", config.width);
    AMediaFormat_setInt32(format, "height", config.height);
    AMediaFormat_setInt32(format, "bitrate", config.bitrate * 1000);
    AMediaFormat_setInt32(format, "frame-rate", config.framerate);
    AMediaFormat_setInt32(format, "operating-rate", config.framerate);
    AMediaFormat_setInt32(format, "i-frame-interval", 100000);
    AMediaFormat_setInt32(format, "color-format", 2130708361);
    AMediaFormat_setInt32(format, "max-bframes", 0);
    bool hdr = false;
    auto colorspace = colorspace_from_client_config(config, hdr);
    AMediaFormat_setInt32(format, "color-standard", colorspace.colorspace == video::colorspace_e::rec601 ? 4 : 1);
    AMediaFormat_setInt32(format, "color-range", colorspace.full_range ? 1 : 2);
    AMediaFormat_setInt32(format, "color-transfer", 3);
    return format;
}


namespace {
    struct JniAttachment {
        JNIEnv *env = nullptr;
        bool attached = false;
        JniAttachment() {
            if (!jvm) throw std::runtime_error("JVM unavailable");
            attached = jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
            if (attached && jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
            if (!env) throw std::runtime_error("JNI attachment failed");
        }
        ~JniAttachment() { if (attached && env) jvm->DetachCurrentThread(); }
    };
    int64_t monotonic_ns() {
        timespec value {};
        if (clock_gettime(CLOCK_MONOTONIC, &value)) throw std::runtime_error("Monotonic clock unavailable");
        return int64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
    }
    jmethodID bridge_method(JNIEnv *env, const char *name, const char *signature) {
        auto method = env->GetStaticMethodID(sunshineServerClass, name, signature);
        if (!method || env->ExceptionCheck()) {
            env->ExceptionClear();
            throw std::runtime_error(std::string("Missing media bridge: ") + name);
        }
        return method;
    }
    void check_java(JNIEnv *env) {
        if (env->ExceptionCheck()) { env->ExceptionClear(); throw std::runtime_error("Java media operation failed"); }
    }
    std::atomic_int audio_session_id {0};
}
namespace sunshine_callbacks {
    static void notifyHost(const char *state, const std::string &reason) {
        JNIEnv *env = nullptr;
        bool attached = jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED;
        if (attached && jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        if (!env || !sunshineServerClass) return;
        auto method = env->GetStaticMethodID(sunshineServerClass, "onNativeHostState", "(Ljava/lang/String;Ljava/lang/String;)V");
        if (method) {
            auto javaState = env->NewStringUTF(state);
            auto javaReason = env->NewStringUTF(reason.c_str());
            env->CallStaticVoidMethod(sunshineServerClass, method, javaState, javaReason);
            env->DeleteLocalRef(javaState);
            env->DeleteLocalRef(javaReason);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (attached) jvm->DetachCurrentThread();
    }
    void hostListenerReady(unsigned listener) {
        std::lock_guard lock(host_mutex);
        if (!host_active || startup_failed || mail::man->event<bool>(mail::shutdown)->peek()) return;
        unsigned previous = ready_listeners;
        ready_listeners |= listener;
        if (previous != 7 && ready_listeners == 7) notifyHost("RUNNING", "");
    }
    void hostStartupFailed(const std::string &reason) {
        std::lock_guard lock(host_mutex);
        if (!host_active) return;
        if (!startup_failed) { startup_failed = true; notifyHost("FAILED", reason); }
        mail::man->event<bool>(mail::shutdown)->raise(true);
    }


    void callJavaOnPinRequested() {
        invokeJavaFunction("onPinRequested", "()V");
    }

    bool createVirtualDisplay(JNIEnv *env, jint width, jint height, jobject surface) {
        jmethodID method = env->GetStaticMethodID(sunshineServerClass,
                "createVirtualDisplay", "(IILandroid/view/Surface;)Z");
        if (!method) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
        bool success = env->CallStaticBooleanMethod(sunshineServerClass, method, width, height, surface);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return success;
    }

    void stopVirtualDisplay() {
        invokeJavaFunction("stopVirtualDisplay", "()V");
    }


    void initializeVideoCapabilities() {
        JniAttachment attachment;
        auto *env = attachment.env;
        auto snapshot = env->CallStaticObjectMethod(sunshineServerClass,
            bridge_method(env, "queryVideoCapabilities", "()Lcom/nightmare/sunshine/VideoCapabilities;"));
        check_java(env);
        if (!snapshot) throw std::runtime_error("Encoder capabilities unavailable");
        auto cleanup = util::fail_guard([&] { env->DeleteLocalRef(snapshot); });
        auto type = env->GetObjectClass(snapshot);
        auto type_cleanup = util::fail_guard([&] { env->DeleteLocalRef(type); });
        auto avc = env->GetFieldID(type, "avc", "Z");
        auto hevc = env->GetFieldID(type, "hevc", "Z");
        check_java(env);
        if (!avc || !hevc) throw std::runtime_error("Invalid encoder snapshot");
        if (!env->GetBooleanField(snapshot, avc)) throw std::runtime_error("No hardware AVC surface encoder");
        video::active_hevc_mode = env->GetBooleanField(snapshot, hevc) ? 2 : 1;
        video::active_av1_mode = 1;
        video::last_encoder_probe_supported_ref_frames_invalidation = false;
        video::last_encoder_probe_supported_yuv444_for_codec.fill(false);
    }

    bool supportsVideo(const video::config_t &config) {
        if (config.videoFormat < 0 || config.videoFormat > 1 || config.dynamicRange || config.chromaSamplingType ||
            config.width <= 0 || config.height <= 0 || config.framerate <= 0 || config.bitrate <= 0 || config.bitrate > INT32_MAX / 1000) return false;
        try {
            JniAttachment attachment;
            auto *env = attachment.env;
            auto name = env->CallStaticObjectMethod(sunshineServerClass,
                bridge_method(env, "selectVideoEncoder", "(IIIII)Ljava/lang/String;"),
                config.videoFormat, config.width, config.height, config.framerate, config.bitrate);
            check_java(env);
            bool supported = name != nullptr;
            if (name) env->DeleteLocalRef(name);
            return supported;
        } catch (const std::exception &exception) { BOOST_LOG(error) << exception.what(); return false; }
    }

    void captureVideoLoop(void *channel_data, safe::mail_t mail, const video::config_t &config,
                          const safe::mail_raw_t::queue_t<video::packet_t> &packets,
                          std::shared_ptr<input::input_t> &input) {
        auto shutdown = mail->event<bool>(mail::shutdown);
        auto failure = util::fail_guard([&] { shutdown->raise(true); });
        try {
            if (!supportsVideo(config)) throw std::runtime_error("Unsupported encoder configuration");
            JniAttachment attachment;
            auto *env = attachment.env;
            auto name = static_cast<jstring>(env->CallStaticObjectMethod(sunshineServerClass,
                bridge_method(env, "selectVideoEncoder", "(IIIII)Ljava/lang/String;"),
                config.videoFormat, config.width, config.height, config.framerate, config.bitrate));
            check_java(env);
            if (!name) throw std::runtime_error("Encoder selection failed");
            auto name_cleanup = util::fail_guard([&] { env->DeleteLocalRef(name); });
            auto utf = env->GetStringUTFChars(name, nullptr);
            if (!utf) { check_java(env); throw std::runtime_error("Encoder name unavailable"); }
            std::string codec_name(utf);
            env->ReleaseStringUTFChars(name, utf);
            auto profile = env->CallStaticIntMethod(sunshineServerClass,
                bridge_method(env, "profileForEncoder", "(Ljava/lang/String;I)I"), name, config.videoFormat);
            auto low_latency = env->CallStaticBooleanMethod(sunshineServerClass,
                bridge_method(env, "lowLatencyForEncoder", "(Ljava/lang/String;)Z"), name);
            check_java(env);
            if (!profile) throw std::runtime_error("Encoder profile unavailable");
            std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> format(createFormat(config), AMediaFormat_delete);
            AMediaFormat_setInt32(format.get(), "profile", profile);
            if (low_latency) AMediaFormat_setInt32(format.get(), "low-latency", 1);
            auto *codec = AMediaCodec_createCodecByName(codec_name.c_str());
            if (!codec) throw std::runtime_error("Encoder creation failed");
            bool started = false;
            auto codec_cleanup = util::fail_guard([&] { if (started) AMediaCodec_stop(codec); AMediaCodec_delete(codec); });
            if (AMediaCodec_configure(codec, format.get(), nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK)
                throw std::runtime_error("Encoder configuration rejected");
            ANativeWindow *window = nullptr;
            if (AMediaCodec_createInputSurface(codec, &window) != AMEDIA_OK || !window)
                throw std::runtime_error("Encoder surface unavailable");
            auto window_cleanup = util::fail_guard([&] { ANativeWindow_release(window); });
            auto surface = ANativeWindow_toSurface(env, window);
            if (!surface) throw std::runtime_error("Java encoder surface unavailable");
            auto surface_cleanup = util::fail_guard([&] { env->DeleteLocalRef(surface); });
            // Start the consumer before preparing its public ImageWriter producer. The output
            // drain below remains independent of the Java renderer's sole GL owner.
            if (AMediaCodec_start(codec) != AMEDIA_OK) throw std::runtime_error("Encoder start rejected");
            started = true;
            auto renderer_cleanup = util::fail_guard([&] { invokeJavaFunction("stopCaptureRenderer", "()V"); });
            auto ingress = env->CallStaticObjectMethod(sunshineServerClass,
                bridge_method(env, "prepareCaptureRenderer", "(Landroid/view/Surface;III)Landroid/view/Surface;"),
                surface, config.width, config.height, config.encoderCscMode);
            check_java(env);
            if (!ingress) throw std::runtime_error("Capture renderer ingress unavailable");
            auto ingress_cleanup = util::fail_guard([&] { env->DeleteLocalRef(ingress); });
            auto display_cleanup = util::fail_guard([&] { stopVirtualDisplay(); });
            if (!createVirtualDisplay(env, config.width, config.height, ingress)) throw std::runtime_error("Projection display creation failed");
            auto geometry = static_cast<jintArray>(env->CallStaticObjectMethod(sunshineServerClass,
                bridge_method(env, "getCaptureGeometry", "()[I")));
            check_java(env);
            if (!geometry) throw std::runtime_error("Physical capture geometry unavailable");
            auto geometry_cleanup = util::fail_guard([&] { env->DeleteLocalRef(geometry); });
            if (env->GetArrayLength(geometry) != 4) throw std::runtime_error("Invalid capture geometry");
            jint values[4]; env->GetIntArrayRegion(geometry, 0, 4, values); check_java(env);
            if (values[1] <= 0 || values[2] <= 0) throw std::runtime_error("Invalid physical display dimensions");
            input::configure(input, values[0], values[1], values[2], values[3]);
            float scale = std::min(float(config.width) / values[1], float(config.height) / values[2]);
            input::touch_port_t port {{0, 0, config.width, config.height}, values[1], values[2],
                (config.width - values[1] * scale) / 2, (config.height - values[2] * scale) / 2, 1 / scale};
            mail->event<input::touch_port_t>(mail::touch_port)->raise(port);
            auto renderer_health = bridge_method(env, "captureRendererHealthy", "()Z");
            android_media::BitstreamAssembler assembler(config.videoFormat == 1);
            auto paired = std::chrono::steady_clock::now();
            android_media::PtsMapper pts(monotonic_ns(), paired);
            auto idr = mail->event<bool>(mail::idr);
            int64_t frame_index = 0;
            unsigned missing_headers = 0;
            bool latency_warning = false;
            bool awaiting_keyframe = true;
            auto request_idr = [&] {
                std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> params(AMediaFormat_new(), AMediaFormat_delete);
                if (!params) throw std::runtime_error("IDR format allocation failed");
                AMediaFormat_setInt32(params.get(), "request-sync", 0);
                if (AMediaCodec_setParameters(codec, params.get()) != AMEDIA_OK) throw std::runtime_error("IDR request rejected");
            };
            while (!shutdown->peek()) {
                if (!env->CallStaticBooleanMethod(sunshineServerClass, renderer_health))
                    throw std::runtime_error("nativeYUV renderer failed; refusing corrupted-buffer fallback");
                check_java(env);
                if (idr->peek()) { idr->pop(); request_idr(); }
                AMediaCodecBufferInfo codec_info {};
                auto wait_start = std::chrono::steady_clock::now();
                auto index = AMediaCodec_dequeueOutputBuffer(codec, &codec_info, 20000);
                auto acquired = std::chrono::steady_clock::now();
                if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
                if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                    std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> output(AMediaCodec_getOutputFormat(codec), AMediaFormat_delete);
                    if (!output) throw std::runtime_error("Missing output format");
                    assembler.clear_configuration();
                    awaiting_keyframe = true;
                    for (const auto *key : {"csd-0", "csd-1", "csd-2"}) {
                        void *data = nullptr; size_t size = 0;
                        if (AMediaFormat_getBuffer(output.get(), key, &data, &size) && size) assembler.append_configuration(static_cast<uint8_t *>(data), size);
                    }
                    request_idr();
                    continue;
                }
                if (index < 0) throw std::runtime_error("Encoder output error");
                bool released = false;
                auto release = util::fail_guard([&] { if (!released) AMediaCodec_releaseOutputBuffer(codec, index, false); });
                size_t capacity = 0;
                auto *data = AMediaCodec_getOutputBuffer(codec, index, &capacity);
                if (codec_info.offset < 0 || codec_info.size < 0 || size_t(codec_info.offset) > capacity || size_t(codec_info.size) > capacity - size_t(codec_info.offset) || (!data && codec_info.size))
                    throw std::runtime_error("Malformed codec output range");
                std::vector<uint8_t> owned;
                bool keyframe = (codec_info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
                if (codec_info.size && (codec_info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
                    assembler.append_configuration(data + codec_info.offset, codec_info.size);
                } else if (codec_info.size) {
                    try {
                        if (keyframe || !awaiting_keyframe) owned = assembler.assemble(data, capacity, codec_info.offset, codec_info.size, keyframe);
                        if (keyframe && !owned.empty()) { missing_headers = 0; awaiting_keyframe = false; }
                    }
                    catch (const android_media::MissingParameterSets &) {
                        awaiting_keyframe = true;
                        if (++missing_headers > 3) throw std::runtime_error("Encoder never supplied keyframe parameter sets");
                        request_idr();
                    }
                }
                if (AMediaCodec_releaseOutputBuffer(codec, index, false) != AMEDIA_OK) throw std::runtime_error("Codec output release failed");
                released = true;
                auto copied = std::chrono::steady_clock::now();
                if (!owned.empty() && !shutdown->peek()) {
                    auto timestamp = pts.map(codec_info.presentationTimeUs, monotonic_ns());
                    if (!timestamp && !latency_warning) { latency_warning = true; BOOST_LOG(info) << "Capture latency unavailable: unverified codec PTS domain"; }
                    stream::postFrame(std::move(owned), ++frame_index, keyframe, channel_data, packets, timestamp);
                    auto handed = std::chrono::steady_clock::now();
                    BOOST_LOG(verbose) << "Codec timing us: wait=" << std::chrono::duration_cast<std::chrono::microseconds>(acquired - wait_start).count()
                        << " hold=" << std::chrono::duration_cast<std::chrono::microseconds>(copied - acquired).count()
                        << " handoff=" << std::chrono::duration_cast<std::chrono::microseconds>(handed - copied).count();
                }
                if (codec_info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
            }
        } catch (const std::exception &exception) { BOOST_LOG(error) << "Capture stopped: " << exception.what(); }
    }

    void captureAudioLoop(void *channel_data, safe::mail_t mail, const audio::config_t &config) {
        audio::capture_android(channel_data, mail, config, jvm, sunshineServerClass, ++audio_session_id);
    }

}