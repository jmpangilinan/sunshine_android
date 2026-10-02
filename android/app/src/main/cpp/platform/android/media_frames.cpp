#include "media_frames.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace android_media {
namespace {
using Nals = std::vector<std::vector<uint8_t>>;
[[noreturn]] void invalid() { throw std::invalid_argument("malformed media buffer"); }
size_t prefix(const uint8_t *p, size_t n) {
  if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
  if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
  return 0;
}
unsigned type(const std::vector<uint8_t> &nal, bool hevc) {
  if (nal.size() < (hevc ? 2u : 1u) || (nal[0] & 0x80)) invalid();
  if (hevc && !(nal[1] & 7)) invalid();
  return hevc ? ((nal[0] >> 1) & 63) : (nal[0] & 31);
}
bool parameter(unsigned t, bool hevc) {
  return hevc ? t >= 32 && t <= 34 : t == 7 || t == 8;
}
Nals annex(const uint8_t *p, size_t n) {
  Nals result;
  size_t pos = 0;
  while (pos < n) {
    size_t start = prefix(p + pos, n - pos);
    if (!start) invalid();
    pos += start;
    size_t end = pos;
    while (end < n && !prefix(p + end, n - end)) ++end;
    size_t payload_end = end;
    // Annex B trailing_zero_8bits are not part of the NAL unit.
    while (payload_end > pos && p[payload_end - 1] == 0) --payload_end;
    if (payload_end == pos) invalid();
    result.emplace_back(p + pos, p + payload_end);
    pos = end;
  }
  return result;
}
struct Reader {
  const uint8_t *p;
  size_t n;
  size_t pos = 0;
  uint8_t byte() { if (pos == n) invalid(); return p[pos++]; }
  size_t word() { size_t hi = byte(); return (hi << 8) | byte(); }
  std::vector<uint8_t> nal() {
    size_t len = word();
    if (!len || len > n - pos) invalid();
    std::vector<uint8_t> out(p + pos, p + pos + len);
    pos += len;
    return out;
  }
};
Nals config(const uint8_t *p, size_t n, bool hevc, unsigned &width) {
  if (!p || !n) invalid();
  if (prefix(p, n)) return annex(p, n);
  Reader r {p, n};
  if (r.byte() != 1) invalid();
  Nals out;
  if (hevc) {
    if (n < 23) invalid();
    r.pos = 21;
    width = (r.byte() & 3) + 1;
    unsigned arrays = r.byte();
    for (unsigned a = 0; a < arrays; ++a) {
      unsigned declared = r.byte() & 63;
      size_t count = r.word();
      for (size_t i = 0; i < count; ++i) {
        auto nal = r.nal();
        if (type(nal, true) != declared) invalid();
        out.push_back(std::move(nal));
      }
    }
  } else {
    if (n < 7) invalid();
    unsigned profile = r.byte();
    r.byte(); r.byte();
    unsigned lengths = r.byte();
    if ((lengths & 0xfc) != 0xfc) invalid();
    width = (lengths & 3) + 1;
    if (width == 3) invalid(); // Reserved by AVCDecoderConfigurationRecord.
    unsigned count_byte = r.byte();
    if ((count_byte & 0xe0) != 0xe0) invalid();
    for (unsigned i = 0; i < (count_byte & 31); ++i) {
      auto nal = r.nal();
      if (type(nal, false) != 7) invalid();
      out.push_back(std::move(nal));
    }
    unsigned count = r.byte();
    for (unsigned i = 0; i < count; ++i) {
      auto nal = r.nal();
      if (type(nal, false) != 8) invalid();
      out.push_back(std::move(nal));
    }
    if (r.pos < n) {
      if (profile != 100 && profile != 110 && profile != 122 && profile != 144 &&
          profile != 44 && profile != 83 && profile != 86 && profile != 118 &&
          profile != 128 && profile != 138 && profile != 139 && profile != 134 && profile != 135) invalid();
      if ((r.byte() & 0xfc) != 0xfc || (r.byte() & 0xf8) != 0xf8 ||
          (r.byte() & 0xf8) != 0xf8) invalid();
      unsigned ext = r.byte();
      for (unsigned i = 0; i < ext; ++i) {
        auto nal = r.nal();
        if (type(nal, false) != 13) invalid();
      }
    }
  }
  if (r.pos != n || out.empty()) invalid();
  return out;
}
struct NalView { const uint8_t *data; size_t size; };
unsigned view_type(NalView nal, bool hevc) {
  if (nal.size < (hevc ? 2u : 1u) || (nal.data[0] & 0x80)) invalid();
  if (hevc && !(nal.data[1] & 7)) invalid();
  return hevc ? ((nal.data[0] >> 1) & 63) : (nal.data[0] & 31);
}
std::vector<NalView> frame(const uint8_t *p, size_t n, unsigned width) {
  std::vector<NalView> out;
  size_t pos = 0;
  // A four-byte length of one is identical to an Annex B start code. Prefer
  // the configured record framing when the complete buffer satisfies it.
  if (width) {
    size_t cursor = 0;
    bool valid = true;
    while (cursor < n) {
      if (width > n - cursor) { valid = false; break; }
      size_t len = 0;
      for (unsigned i = 0; i < width; ++i) len = (len << 8) | p[cursor++];
      if (!len || len > n - cursor) { valid = false; break; }
      out.push_back({p + cursor, len});
      cursor += len;
    }
    if (valid) return out;
    out.clear();
  }
  const bool is_annex = prefix(p, n) != 0;
  if (!is_annex && !width) invalid();
  while (pos < n) {
    if (is_annex) {
      size_t start = prefix(p + pos, n - pos);
      if (!start) invalid();
      pos += start;
      size_t end = pos;
      while (end < n && !prefix(p + end, n - end)) ++end;
      size_t payload_end = end;
      while (payload_end > pos && p[payload_end - 1] == 0) --payload_end;
      if (payload_end == pos) invalid();
      out.push_back({p + pos, payload_end - pos});
      pos = end;
    } else {
      if (width > n - pos) invalid();
      size_t len = 0;
      for (unsigned i = 0; i < width; ++i) len = (len << 8) | p[pos++];
      if (!len || len > n - pos) invalid();
      out.push_back({p + pos, len});
      pos += len;
    }
  }
  return out;
}
} // namespace

BitstreamAssembler::BitstreamAssembler(bool hevc) : hevc_(hevc) {}
void BitstreamAssembler::clear_configuration() noexcept {
  parameter_sets_.clear();
  length_bytes_ = 0;
}
void BitstreamAssembler::set_configuration(const uint8_t *p, size_t n) {
  clear_configuration();
  append_configuration(p, n);
}
void BitstreamAssembler::append_configuration(const uint8_t *p, size_t n) {
  unsigned width = length_bytes_;
  auto nals = config(p, n, hevc_, width);
  bool found = false;
  for (const auto &nal : nals) found |= parameter(type(nal, hevc_), hevc_);
  if (!found) invalid();
  // Parse and validate the entire buffer before mutating configuration.
  for (auto &nal : nals) {
    if (parameter(type(nal, hevc_), hevc_) &&
        std::find(parameter_sets_.begin(), parameter_sets_.end(), nal) == parameter_sets_.end())
      parameter_sets_.push_back(std::move(nal));
  }
  length_bytes_ = width;
}
std::vector<uint8_t> BitstreamAssembler::assemble(const uint8_t *data, size_t capacity,
                                                size_t offset, size_t size, bool keyframe) const {
  if (!data || !size || offset > capacity || size > capacity - offset) invalid();
  auto nals = frame(data + offset, size, length_bytes_);
  bool payload = false;
  for (const auto &nal : nals) {
    unsigned t = view_type(nal, hevc_);
    payload |= hevc_ ? t <= 31 : t >= 1 && t <= 5;
  }
  if (!payload) invalid(); // Configuration alone is not a video frame.
  std::vector<NalView> selected;
  if (keyframe) {
    const unsigned first = hevc_ ? 32 : 7;
    const unsigned last = hevc_ ? 34 : 8;
    for (unsigned t = first; t <= last; ++t) {
      bool found = false;
      // In-band sets take precedence over cached sets of the same type. Move
      // all required sets before slices in decoder dependency order.
      for (auto nal : nals) {
        if (view_type(nal, hevc_) != t) continue;
        const bool duplicate = std::any_of(selected.begin(), selected.end(),
            [nal](NalView old) {
              return old.size == nal.size &&
                     std::memcmp(old.data, nal.data, nal.size) == 0;
            });
        if (!duplicate) selected.push_back(nal);
        found = true;
      }
      if (!found) {
        for (const auto &nal : parameter_sets_) {
          if (type(nal, hevc_) == t) {
            selected.push_back({nal.data(), nal.size()});
            found = true;
          }
        }
      }
      if (!found) throw MissingParameterSets();
    }
  }
  for (auto nal : nals) {
    if (!keyframe || !parameter(view_type(nal, hevc_), hevc_))
      selected.push_back(nal);
  }
  size_t total = 0;
  for (auto nal : selected) {
    if (nal.size > std::numeric_limits<size_t>::max() - 4 ||
        total > std::numeric_limits<size_t>::max() - 4 - nal.size) invalid();
    total += 4 + nal.size;
  }
  std::vector<uint8_t> result;
  result.reserve(total);
  for (auto nal : selected) {
    result.insert(result.end(), {0, 0, 0, 1});
    result.insert(result.end(), nal.data, nal.data + nal.size);
  }
  return result;
}

PcmPacketizer::PcmPacketizer(size_t packet_samples) : packet_samples_(packet_samples) {
  if (!packet_samples || packet_samples > partial_.max_size()) invalid();
  partial_.reserve(packet_samples);
}
void PcmPacketizer::push(const float *samples, size_t count) {
  if (!samples && count) invalid();
  while (count) {
    size_t take = std::min(count, packet_samples_ - partial_.size());
    partial_.insert(partial_.end(), samples, samples + take);
    samples += take;
    count -= take;
    if (partial_.size() == packet_samples_) {
      std::vector<float> recycled;
      if (packets_.size() == 4) {
        recycled = std::move(packets_.front());
        packets_.pop_front();
        recycled.clear();
      } else {
        recycled.reserve(packet_samples_);
      }
      packets_.push_back(std::move(partial_));
      partial_ = std::move(recycled);
    }
  }
}
std::optional<std::vector<float>> PcmPacketizer::pop() {
  if (packets_.empty()) return std::nullopt;
  auto packet = std::move(packets_.front());
  packets_.pop_front();
  return packet;
}
void PcmPacketizer::reset() { packets_.clear(); partial_.clear(); }

PtsMapper::PtsMapper(int64_t monotonic_ns, clock::time_point steady_sample,
                     std::chrono::microseconds maximum_age)
    : monotonic_ns_(monotonic_ns), steady_sample_(steady_sample),
      maximum_age_us_(maximum_age.count()) {
  if (monotonic_ns < 0 || maximum_age_us_ < 0) invalid();
}
std::optional<PtsMapper::clock::time_point> PtsMapper::map(int64_t pts_us, int64_t now_ns) {
  const auto previous = previous_pts_;
  if (pts_us >= 0 && (!previous || pts_us > *previous)) previous_pts_ = pts_us;
  if (pts_us < 0 || now_ns < monotonic_ns_ ||
      (previous && pts_us <= *previous)) return std::nullopt;
  const int64_t now_us = now_ns / 1000;
  // Startup paired sample must precede this session's capture. Relative epochs
  // cannot pass this check, even if their deltas are monotonic.
  if (pts_us < monotonic_ns_ / 1000 || pts_us > now_us ||
      now_us - pts_us > maximum_age_us_) return std::nullopt;
  if (pts_us > std::numeric_limits<int64_t>::max() / 1000) return std::nullopt;
  const auto delta = std::chrono::nanoseconds(pts_us * 1000 - monotonic_ns_);
  const auto ticks = std::chrono::duration_cast<clock::duration>(delta);
  if (ticks.count() > 0 && steady_sample_ > clock::time_point::max() - ticks) return std::nullopt;
  if (ticks.count() < 0 && steady_sample_ < clock::time_point::min() - ticks) return std::nullopt;
  return steady_sample_ + ticks;
}
void PtsMapper::reset() noexcept { previous_pts_.reset(); }
} // namespace android_media
