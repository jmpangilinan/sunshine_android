#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>
#include <stdexcept>

namespace android_media {

class MissingParameterSets : public std::runtime_error {
 public:
  MissingParameterSets() : std::runtime_error("keyframe lacks required parameter sets") {}
};

// All encoded output is Annex B. Invalid/truncated input throws invalid_argument;
// a keyframe without the codec's complete parameter-set types is rejected.
// Keyframes merge in-band sets with missing cached types in dependency order
// (AVC SPS/PPS, HEVC VPS/SPS/PPS), before payload. Duplicate in-band sets are
// emitted once; in-band types take precedence over stale cached types.
class BitstreamAssembler {
 public:
  explicit BitstreamAssembler(bool hevc);
  // Replaces all configuration (including length-prefix width). Failure leaves
  // the old configuration cleared, so a new format cannot reuse stale headers.
  void set_configuration(const uint8_t *buffer, size_t size);
  void clear_configuration() noexcept;
  // Adds configuration buffers from the same format; does not replace headers.
  void append_configuration(const uint8_t *buffer, size_t size);
  std::vector<uint8_t> assemble(const uint8_t *data, size_t capacity,
                              size_t offset, size_t size, bool keyframe) const;

 private:
  bool hevc_;
  unsigned length_bytes_ = 0;
  std::vector<std::vector<uint8_t>> parameter_sets_;
};

// Samples, not bytes or per-channel frames: stereo packets contain twice the
// sample-frame count. Keeps partial reads and at most four complete packets;
// overflow discards only the oldest complete packet.
class PcmPacketizer {
 public:
  explicit PcmPacketizer(size_t packet_samples);
  void push(const float *samples, size_t count);
  std::optional<std::vector<float>> pop();
  size_t queued_packets() const noexcept { return packets_.size(); }
  size_t pending_samples() const noexcept { return partial_.size(); }
  // A read of at most packet_samples cannot overflow while this is true,
  // even when a previous short read left a partial packet.
  bool can_accept_packet() const noexcept { return packets_.size() < 4; }
  void reset();

 private:
  size_t packet_samples_;
  std::vector<float> partial_;
  std::deque<std::vector<float>> packets_;
};

// The caller samples CLOCK_MONOTONIC and steady_clock together, before output.
// Only PTS demonstrably in that absolute domain is mapped. A relative epoch,
// negative age, stale sample, or non-increasing PTS returns nullopt; there is no
// anchoring to the first output. Call reset for a new codec session.
class PtsMapper {
 public:
  using clock = std::chrono::steady_clock;
  PtsMapper(int64_t monotonic_ns, clock::time_point steady_sample,
            std::chrono::microseconds maximum_age = std::chrono::seconds(5));
  std::optional<clock::time_point> map(int64_t pts_us, int64_t now_monotonic_ns);
  void reset() noexcept;

 private:
  int64_t monotonic_ns_;
  clock::time_point steady_sample_;
  int64_t maximum_age_us_;
  std::optional<int64_t> previous_pts_;
};

} // namespace android_media
