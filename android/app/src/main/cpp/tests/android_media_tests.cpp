#include "../platform/android/media_frames.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace android_media;
namespace {
void check(bool value) { if (!value) throw std::runtime_error("media boundary assertion failed"); }
template<class E = std::invalid_argument, class F> void rejects(F f) {
  bool caught = false;
  try { f(); } catch (const E &) { caught = true; }
  check(caught);
}
std::vector<uint8_t> avc_config() {
  return {1, 66, 0, 30, 255, 225, 0, 2, 0x67, 0x11, 1, 0, 2, 0x68, 0x22};
}
void bitstreams() {
  BitstreamAssembler avc(false);
  auto cfg = avc_config();
  avc.set_configuration(cfg.data(), cfg.size());
  const std::vector<uint8_t> encoded {99, 98, 0, 0, 0, 2, 0x65, 0xab, 97};
  const uint8_t ambiguous[] {0,0,0,1,0x41,0,0,0,1,0x41};
  check(avc.assemble(ambiguous, sizeof ambiguous, 0, sizeof ambiguous, false) ==
        std::vector<uint8_t>({0,0,0,1,0x41,0,0,0,1,0x41}));
  const std::vector<uint8_t> expected {0,0,0,1,0x67,0x11,0,0,0,1,0x68,0x22,0,0,0,1,0x65,0xab};
  check(avc.assemble(encoded.data(), encoded.size(), 2, 6, true) == expected);
  const std::vector<uint8_t> delta {0,0,0,2,0x41,0xcd};
  check(avc.assemble(delta.data(), delta.size(), 0, delta.size(), false) ==
        std::vector<uint8_t>({0,0,0,1,0x41,0xcd}));
  const std::vector<uint8_t> inband {0,0,1,0x67,0x11,0,0,1,0x65,0xab};
  check(avc.assemble(inband.data(), inband.size(), 0, inband.size(), true) == expected);
  // A fresh decoder must see SPS before PPS before the dependent IDR even
  // when headers occur late, reversed, or duplicated in codec output.
  const std::vector<uint8_t> reversed {0,0,1,0x65,0xab,0,0,1,0x68,0x33,
                                      0,0,1,0x67,0x44,0,0,1,0x68,0x33};
  check(avc.assemble(reversed.data(), reversed.size(), 0, reversed.size(), true) ==
        std::vector<uint8_t>({0,0,0,1,0x67,0x44,0,0,0,1,0x68,0x33,0,0,0,1,0x65,0xab}));
  rejects([&] { avc.assemble(encoded.data(), encoded.size(), 2, 100, true); });
  rejects([&] { avc.assemble(encoded.data(), encoded.size(), std::numeric_limits<size_t>::max(), 4, true); });
  rejects([&] { avc.assemble(encoded.data(), encoded.size(), 0, 0, true); });
  rejects([&] { avc.assemble(nullptr, 100, 0, 4, true); });
  for (size_t n = 1; n < cfg.size(); ++n)
    rejects([&] { BitstreamAssembler b(false); b.set_configuration(cfg.data(), n); });
  const uint8_t malformed[] {0,0,0,8,0x65,1};
  rejects([&] { avc.assemble(malformed, sizeof malformed, 0, sizeof malformed, false); });
  const uint8_t zero[] {0,0,0,0};
  rejects([&] { avc.assemble(zero, sizeof zero, 0, sizeof zero, false); });
  const uint8_t sps[] {0,0,1,0x67,0x11};
  const uint8_t pps[] {0,0,1,0x68,0x22};
  avc.clear_configuration();
  avc.append_configuration(sps, sizeof sps);
  rejects<MissingParameterSets>([&] { avc.assemble(inband.data(), inband.size(), 0, inband.size(), true); });
  avc.append_configuration(pps, sizeof pps);
  check(avc.assemble(inband.data(), inband.size(), 0, inband.size(), true).size() == 18);
  rejects([&] { avc.assemble(sps, sizeof sps, 0, sizeof sps, false); });
  rejects([&] { avc.set_configuration(nullptr, 0); });
  rejects<MissingParameterSets>([&] { avc.assemble(inband.data(), inband.size(), 0, inband.size(), true); });

  BitstreamAssembler hevc(true);
  std::vector<uint8_t> hvcc(23, 0);
  hvcc[0] = 1; hvcc[21] = 3; hvcc[22] = 3;
  for (unsigned t = 32; t <= 34; ++t)
    hvcc.insert(hvcc.end(), {static_cast<uint8_t>(t), 0, 1, 0, 3,
                            static_cast<uint8_t>(t << 1), 1, static_cast<uint8_t>(t)});
  hevc.set_configuration(hvcc.data(), hvcc.size());
  const uint8_t hframe[] {0,0,0,3,0x26,1,0x55};
  check(hevc.assemble(hframe, sizeof hframe, 0, sizeof hframe, true) ==
        std::vector<uint8_t>({0,0,0,1,0x40,1,32,0,0,0,1,0x42,1,33,
                              0,0,0,1,0x44,1,34,0,0,0,1,0x26,1,0x55}));
  const std::vector<uint8_t> mixed_hevc {0,0,1,0x26,1,0x55,
                                        0,0,1,0x44,1,0x77,0,0,1,0x42,1,0x66,
                                        0,0,1,0x42,1,0x66};
  check(hevc.assemble(mixed_hevc.data(), mixed_hevc.size(), 0, mixed_hevc.size(), true) ==
        std::vector<uint8_t>({0,0,0,1,0x40,1,32,0,0,0,1,0x42,1,0x66,
                             0,0,0,1,0x44,1,0x77,0,0,0,1,0x26,1,0x55}));
  for (size_t n = 1; n < hvcc.size(); ++n)
    rejects([&] { BitstreamAssembler b(true); b.set_configuration(hvcc.data(), n); });
  hvcc.push_back(0);
  rejects([&] { hevc.append_configuration(hvcc.data(), hvcc.size()); });
  // Failed append is transactional: prior headers still assemble a keyframe.
  check(hevc.assemble(hframe, sizeof hframe, 0, sizeof hframe, true).size() == 28);
}
void pcm() {
  rejects([] { PcmPacketizer invalid(0); });
  PcmPacketizer p(4);
  const float first[] {0,1,2};
  p.push(first, 3);
  check(!p.pop() && p.pending_samples() == 3);
  const float next[] {3,4,5,6,7,8};
  p.push(next, 6);
  check(p.pop() == std::optional<std::vector<float>>(std::vector<float>{0,1,2,3}));
  check(p.pop() == std::optional<std::vector<float>>(std::vector<float>{4,5,6,7}));
  check(!p.pop() && p.pending_samples() == 1);
  std::vector<float> tail;
  for (int i = 9; i <= 30; ++i) tail.push_back(static_cast<float>(i));
  p.push(tail.data(), tail.size());
  check(p.queued_packets() == 4 && p.pending_samples() == 3);
  for (int start = 12; start <= 24; start += 4)
    check(p.pop() == std::optional<std::vector<float>>(std::vector<float>{float(start),float(start+1),float(start+2),float(start+3)}));
  const float last = 31;
  p.push(&last, 1);
  check(p.pop() == std::optional<std::vector<float>>(std::vector<float>{28,29,30,31}));
  p.push(first, 3); p.reset();
  check(!p.pop() && p.pending_samples() == 0);
  p.push(nullptr, 0);
  rejects([&] { p.push(nullptr, 1); });
  // AudioRecord may expose five packets at once. Admit only four; keep the
  // fifth unread until a consumer credit exists instead of dropping packet 0.
  PcmPacketizer burst(4);
  for (int start = 0; start < 16; start += 4) {
    check(burst.can_accept_packet());
    const float packet[] {float(start),float(start+1),float(start+2),float(start+3)};
    burst.push(packet, 1);
    check(burst.can_accept_packet()); // Partial short read reserves no extra packet.
    burst.push(packet + 1, 3);
  }
  check(!burst.can_accept_packet());
  check(burst.pop() == std::optional<std::vector<float>>(std::vector<float>{0,1,2,3}));
  check(burst.can_accept_packet());
  const float fifth[] {16,17,18,19};
  burst.push(fifth, 4);
  for (int start = 4; start < 20; start += 4)
    check(burst.pop() == std::optional<std::vector<float>>(std::vector<float>{float(start),float(start+1),float(start+2),float(start+3)}));
}
void pts() {
  using namespace std::chrono;
  const PtsMapper::clock::time_point steady(seconds(50));
  PtsMapper mapper(10000000000LL, steady);
  check(mapper.map(10001000, 10002000000LL) == steady + milliseconds(1));
  check(!mapper.map(10001000, 10003000000LL)); // Repeated timestamp.
  check(!mapper.map(10000000, 10003000000LL)); // Regression.
  check(!mapper.map(10005000, 10004000000LL)); // Future capture.
  check(!mapper.map(10006000, 20000000000LL)); // Unreasonably old capture.
  mapper.reset();
  check(!mapper.map(0, 10002000000LL)); // Relative epoch, not fabricated latency.
  check(!mapper.map(1000, 10003000000LL));
  mapper.reset();
  check(mapper.map(10000000, 10002000000LL) == steady);
  check(!mapper.map(-1, 10002000000LL));
  rejects([&] { PtsMapper bad(-1, steady); });
}
}
int main() {
  try { bitstreams(); pcm(); pts(); }
  catch (const std::exception &e) { std::cerr << e.what() << '\n'; return EXIT_FAILURE; }
  std::cout << "Android media boundary tests passed\n";
  return EXIT_SUCCESS;
}
