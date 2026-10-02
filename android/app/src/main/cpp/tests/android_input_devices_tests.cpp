#include "../platform/android/input_devices.h"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <string>
using namespace sunshine::input_devices;
int main() {
  assert(invert_y(INT16_MIN)==INT16_MAX);
  assert(invert_y(INT16_MAX)==-32767);
  assert(invert_y(0)==0);
  const uint32_t directions[]={1,1|8,8,8|2,2,2|4,4,4|1};
  for(unsigned i=0;i<8;++i) assert(controller_hat(directions[i])==i);
  assert(controller_hat(0)==8);
  assert(controller_hat(1|2|4|8)==8);
  assert(controller_hat(1|2|8)==2);
  // Linux Generic Game Pad HID buttons map usage N to BTN_GAMEPAD + N - 1.
  // Test every named consumer control, including C/Z and L2/R2 gaps, independently.
  struct NamedButton { const char *name; uint32_t protocol; unsigned hidUsage; };
  const NamedButton buttons[]={
    {"A",0x1000,1},{"B",0x2000,2},{"X",0x4000,4},{"Y",0x8000,5},
    {"LB",0x100,7},{"RB",0x200,8},{"Back",0x20,11},{"Start",0x10,12},
    {"Guide",0x400,13},{"Left stick click",0x40,14},{"Right stick click",0x80,15}
  };
  for(const auto &button:buttons) {
    ControllerState pressed{};pressed.buttons=button.protocol;
    auto report=controller_report(pressed);
    const unsigned delivered=report[1]|(unsigned(report[2])<<8);
    assert(delivered==(1u<<(button.hidUsage-1)));
  }
  ControllerState all{};for(const auto &button:buttons)all.buttons|=button.protocol;
  auto allReport=controller_report(all);
  assert(((allReport[1]|(unsigned(allReport[2])<<8)) & ((1u<<2)|(1u<<5)|(1u<<8)|(1u<<9)|(1u<<15)))==0);
  ControllerState s{0x1000|0x8000|0x400|0x80,255,17,-32768,-32768,32767,32767};
  auto r=controller_report(s);
  assert(r[0]==1 && r[3]==8);
  assert(r[4]==0 && r[5]==128); // X signed little-endian minimum.
  assert(r[6]==255 && r[7]==127); // Positive-up Y saturation.
  assert(r[8]==255 && r[9]==127);
  assert(r[10]==1 && r[11]==128);
  assert(r[12]==255 && r[13]==17);
  std::string error;
  assert(!write_record(-1,r.data(),r.size(),error));
  assert(error=="Virtual input device is closed");
  int pipes[2]; assert(pipe(pipes)==0);
  assert(!write_record(pipes[0],r.data(),r.size(),error)); // Read-only FD rejects without ownership transfer.
  assert(fcntl(pipes[0],F_GETFD)>=0);
  assert(write_record(pipes[1],r.data(),r.size(),error));
  uint8_t received[14]{};assert(read(pipes[0],received,sizeof(received))==14);
  for(unsigned i=0;i<14;++i)assert(received[i]==r[i]);
  close(pipes[0]);close(pipes[1]);
  int full=open("/dev/full",O_WRONLY);assert(full>=0);
  assert(!write_record(full,r.data(),r.size(),error));assert(!error.empty());close(full);
}
