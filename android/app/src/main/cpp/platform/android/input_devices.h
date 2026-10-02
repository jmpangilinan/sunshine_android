#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sunshine::input_devices {
struct ControllerState {
  uint32_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
int16_t invert_y(int16_t value);
uint8_t controller_hat(uint32_t buttons);
std::array<uint8_t, 14> controller_report(const ControllerState &state);
// A failed write never transfers ownership of the descriptor.
bool write_record(int fd, const void *data, size_t size, std::string &error);
}
