#include "input_devices.h"
#include <jni.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/uhid.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace sunshine::input_devices {
int16_t invert_y(int16_t v) { return v == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-v); }
uint8_t controller_hat(uint32_t b) {
  const int x = !!(b & 8) - !!(b & 4), y = !!(b & 2) - !!(b & 1);
  if (!x && !y) return 8;
  if (y < 0) return x < 0 ? 7 : x > 0 ? 1 : 0;
  if (y > 0) return x < 0 ? 5 : x > 0 ? 3 : 4;
  return x > 0 ? 2 : 6;
}
static constexpr uint32_t masks[] = {0x1000,0x2000,0x4000,0x8000,0x100,0x200,0x20,0x10,0x400,0x40,0x80};
static constexpr int keys[] = {BTN_SOUTH,BTN_EAST,BTN_NORTH,BTN_WEST,BTN_TL,BTN_TR,BTN_SELECT,BTN_START,BTN_MODE,BTN_THUMBL,BTN_THUMBR};
std::array<uint8_t,14> controller_report(const ControllerState &s) {
  std::array<uint8_t,14> r{}; r[0]=1;
  for (unsigned i=0;i<11;++i) if(s.buttons&masks[i]) {
    const unsigned bit=keys[i]-BTN_GAMEPAD;
    r[1+bit/8] |= 1u<<(bit%8);
  }
  r[3]=controller_hat(s.buttons);
  const int16_t a[]={s.lx,invert_y(s.ly),s.rx,invert_y(s.ry)};
  for(unsigned i=0;i<4;++i) { r[4+i*2]=static_cast<uint16_t>(a[i]); r[5+i*2]=static_cast<uint16_t>(a[i])>>8; }
  r[12]=s.lt; r[13]=s.rt; return r;
}
bool write_record(int fd,const void *data,size_t n,std::string &error) {
  if(fd<0) { error="Virtual input device is closed"; return false; }
  ssize_t result; do { result=::write(fd,data,n); } while(result<0 && errno==EINTR);
  if(result!=static_cast<ssize_t>(n)) { error=result<0 ? std::strerror(errno) : "Incomplete kernel input record"; return false; }
  return true;
}
namespace {
// HID usages deliberately generic; Android mapping is validated before advertising a controller.
constexpr uint8_t gamepad_desc[]={
  0x05,0x01,0x09,0x05,0xa1,0x01,0x85,0x01,
  0x05,0x09,0x19,0x01,0x29,0x10,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x10,0x81,0x02,
  0x05,0x01,0x09,0x39,0x15,0x00,0x25,0x07,0x35,0x00,0x46,0x3b,0x01,0x65,0x14,0x75,0x04,0x95,0x01,0x81,0x42,
  0x65,0x00,0x75,0x04,0x95,0x01,0x81,0x03,
  0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,0x16,0x00,0x80,0x26,0xff,0x7f,0x75,0x10,0x95,0x04,0x81,0x02,
  // Simulation Controls: Brake -> ABS_BRAKE/LTRIGGER, Accelerator -> ABS_GAS/RTRIGGER.
  0x05,0x02,0x09,0xc5,0x09,0xc4,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x02,0x81,0x02,0xc0};
constexpr uint8_t mouse_desc[]={
  0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x01,0x09,0x01,0xa1,0x00,
  0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x05,0x81,0x02,
  0x75,0x03,0x95,0x01,0x81,0x03,0x05,0x01,0x09,0x30,0x09,0x31,
  0x16,0x00,0x80,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x06,
  0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,
  0x05,0x0c,0x0a,0x38,0x02,0x95,0x01,0x81,0x06,0xc0,0xc0};
struct Device {
  int fd=-1; bool uinput=false; std::string name; ControllerState state;
  std::array<input_event, KEY_MAX + 2> pending{}; size_t pendingCount=0;
  Device()=default; Device(const Device&)=delete; Device& operator=(const Device&)=delete;
  ~Device(){ if(fd>=0){ if(uinput) ioctl(fd,UI_DEV_DESTROY); ::close(fd); } }
};
struct Owner {
  bool root=false; std::string error; std::unique_ptr<Device> mouse,keyboard;
  std::map<int,std::unique_ptr<Device>> controllers;
  uint32_t mouseButtons=0; int wheel=0,pan=0; bool held[KEY_MAX+1]{}; unsigned serial=0;
};
static bool event(Device &d,int type,int code,int value,std::string &error) {
  if(d.pendingCount==d.pending.size()){error="Input frame exceeds event capacity";return false;}
  auto &e=d.pending[d.pendingCount++];e={};e.type=type;e.code=code;e.value=value;
  return true;
}
static bool syn(Device &d,std::string &error) {
  if(!event(d,EV_SYN,SYN_REPORT,0,error))return false;
  const bool ok=write_record(d.fd,d.pending.data(),d.pendingCount*sizeof(input_event),error);
  d.pendingCount=0;return ok;
}
static std::unique_ptr<Device> uinput(Owner &o,int kind,const std::string &name) {
  auto d=std::make_unique<Device>(); d->uinput=true; d->name=name;
  d->fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
  if(d->fd<0) d->fd=open("/dev/input/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
  if(d->fd<0){o.error=std::strerror(errno);return {};}
  auto bit=[&](unsigned long request,int value){return ioctl(d->fd,request,value)>=0;};
  bool ok=bit(UI_SET_EVBIT,EV_KEY);
  if(kind==0){ ok &= bit(UI_SET_EVBIT,EV_REL); for(int c:{REL_X,REL_Y,REL_WHEEL,REL_HWHEEL})ok &=bit(UI_SET_RELBIT,c); for(int c:{BTN_LEFT,BTN_RIGHT,BTN_MIDDLE,BTN_SIDE,BTN_EXTRA})ok &=bit(UI_SET_KEYBIT,c); }
  // Repeats arrive from Moonlight's ordered worker; no independent kernel repeat timer.
  if(kind==1) for(int c=1;c<BTN_MISC;++c)ok &=bit(UI_SET_KEYBIT,c);
  uinput_user_dev legacy{}; std::strncpy(legacy.name,name.c_str(),UINPUT_MAX_NAME_SIZE-1);
  legacy.id.bustype=BUS_VIRTUAL; legacy.id.vendor=0x1209; legacy.id.product=kind==2?0x5355:0x5356;
  if(kind==2){ok &=bit(UI_SET_EVBIT,EV_ABS);for(int c:keys)ok &=bit(UI_SET_KEYBIT,c);
    for(int c:{ABS_X,ABS_Y,ABS_Z,ABS_RZ,ABS_BRAKE,ABS_GAS,ABS_HAT0X,ABS_HAT0Y}) {
      ok &=bit(UI_SET_ABSBIT,c); legacy.absmin[c]=(c==ABS_BRAKE||c==ABS_GAS)?0:(c==ABS_HAT0X||c==ABS_HAT0Y)?-1:-32768;
      legacy.absmax[c]=(c==ABS_BRAKE||c==ABS_GAS)?255:(c==ABS_HAT0X||c==ABS_HAT0Y)?1:32767;
    }
  }
  if(!ok){o.error=std::strerror(errno);return {};}
  uinput_setup setup{}; setup.id=legacy.id; std::strncpy(setup.name,legacy.name,sizeof(setup.name)-1);
  bool modern=ioctl(d->fd,UI_DEV_SETUP,&setup)>=0;
  if(modern && kind==2)for(int c:{ABS_X,ABS_Y,ABS_Z,ABS_RZ,ABS_BRAKE,ABS_GAS,ABS_HAT0X,ABS_HAT0Y}){
    uinput_abs_setup a{};a.code=c;a.absinfo.minimum=legacy.absmin[c];a.absinfo.maximum=legacy.absmax[c];
    if(ioctl(d->fd,UI_ABS_SETUP,&a)<0){modern=false;break;}
  }
  if(!modern && !write_record(d->fd,&legacy,sizeof(legacy),o.error))return {};
  if(ioctl(d->fd,UI_DEV_CREATE)<0){o.error=std::strerror(errno);return {};}
  return d;
}
static std::unique_ptr<Device> uhid(Owner &o,bool controller,const std::string &name) {
  auto d=std::make_unique<Device>(); d->name=name; d->fd=open("/dev/uhid",O_RDWR|O_NONBLOCK|O_CLOEXEC);
  if(d->fd<0){o.error=std::strerror(errno);return {};}
  uhid_event e{};e.type=UHID_CREATE2;std::strncpy(reinterpret_cast<char*>(e.u.create2.name),name.c_str(),sizeof(e.u.create2.name)-1);
  e.u.create2.bus=BUS_VIRTUAL;e.u.create2.vendor=0x1209;e.u.create2.product=controller?0x5355:0x5356;
  const auto *desc=controller?gamepad_desc:mouse_desc;const size_t n=controller?sizeof(gamepad_desc):sizeof(mouse_desc);
  e.u.create2.rd_size=n;std::memcpy(e.u.create2.rd_data,desc,n);
  if(!write_record(d->fd,&e,sizeof(e),o.error))return {};return d;
}
static bool hid_report(Device &d,const uint8_t *data,size_t n,std::string &error) {
  // Drain notification records so a persistent device cannot fill the kernel event queue.
  uhid_event pending{};while(read(d.fd,&pending,sizeof(pending))>0){}
  uhid_event e{};e.type=UHID_INPUT2;e.u.input2.size=n;std::memcpy(e.u.input2.data,data,n);
  return write_record(d.fd,&e,sizeof(e),error);
}
static bool controller(Owner &o,Device &d,const ControllerState &s) {
  if(!d.uinput){auto r=controller_report(s);if(!hid_report(d,r.data(),r.size(),o.error))return false;}
  else {
    for(unsigned i=0;i<11;++i)if((s.buttons^d.state.buttons)&masks[i])if(!event(d,EV_KEY,keys[i],!!(s.buttons&masks[i]),o.error))return false;
    const int code[]={ABS_X,ABS_Y,ABS_Z,ABS_RZ,ABS_BRAKE,ABS_GAS,ABS_HAT0X,ABS_HAT0Y};
    const int now[]={s.lx,invert_y(s.ly),s.rx,invert_y(s.ry),s.lt,s.rt,!!(s.buttons&8)-!!(s.buttons&4),!!(s.buttons&2)-!!(s.buttons&1)};
    const auto &p=d.state;const int old[]={p.lx,invert_y(p.ly),p.rx,invert_y(p.ry),p.lt,p.rt,!!(p.buttons&8)-!!(p.buttons&4),!!(p.buttons&2)-!!(p.buttons&1)};
    for(unsigned i=0;i<8;++i)if(now[i]!=old[i]&&!event(d,EV_ABS,code[i],now[i],o.error))return false;
    if(!syn(d,o.error))return false;
  }d.state=s;return true;
}
static constexpr int mouse_keys[]={BTN_LEFT,BTN_MIDDLE,BTN_RIGHT,BTN_SIDE,BTN_EXTRA};
static bool mouse(Owner &o,int dx,int dy,int buttons,int vertical,int horizontal) {
  if(!o.mouse){o.error="Relative mouse unavailable";return false;}auto &d=*o.mouse;
  // Wide arithmetic prevents malformed protocol values overflowing accumulated remainder.
  int64_t v=static_cast<int64_t>(o.wheel)+vertical,h=static_cast<int64_t>(o.pan)+horizontal;
  o.wheel=v%120;o.pan=h%120;v/=120;h/=120;
  if(d.uinput){
    for(unsigned i=0;i<5;++i)if((o.mouseButtons^buttons)&(1u<<i))if(!event(d,EV_KEY,mouse_keys[i],!!(buttons&(1u<<i)),o.error))return false;
    if(dx&&!event(d,EV_REL,REL_X,dx,o.error))return false;if(dy&&!event(d,EV_REL,REL_Y,dy,o.error))return false;
    if(v&&!event(d,EV_REL,REL_WHEEL,v,o.error))return false;if(h&&!event(d,EV_REL,REL_HWHEEL,h,o.error))return false;
    if(!syn(d,o.error))return false;
  }else{
    // HID limits must split rather than silently lose large movements/wheel values.
    do {int x=std::clamp(dx,-32768,32767),y=std::clamp(dy,-32768,32767),wv=std::clamp<int64_t>(v,-127,127),wh=std::clamp<int64_t>(h,-127,127);
      const uint8_t hidButtons=(buttons&1)|((buttons&2)<<1)|((buttons&4)>>1)|(buttons&24);
      const uint8_t report[]={1,hidButtons,static_cast<uint8_t>(x),static_cast<uint8_t>(x>>8),static_cast<uint8_t>(y),static_cast<uint8_t>(y>>8),static_cast<uint8_t>(wv),static_cast<uint8_t>(wh)};
      if(!hid_report(d,report,sizeof(report),o.error))return false;dx-=x;dy-=y;v-=wv;h-=wh;
    }while(dx||dy||v||h);
  }o.mouseButtons=buttons;return true;
}
static void reset(Owner &o){
  if(o.mouse)mouse(o,0,0,0,0,0);o.wheel=o.pan=0;
  if(o.keyboard){for(int k=0;k<=KEY_MAX;++k)if(o.held[k]){event(*o.keyboard,EV_KEY,k,0,o.error);o.held[k]=false;}syn(*o.keyboard,o.error);}
  for(auto &entry:o.controllers)controller(o,*entry.second,{});
}
static Owner *owner(jlong p){return reinterpret_cast<Owner*>(p);}
}
}
using namespace sunshine::input_devices;
#define JNI_METHOD(name) Java_com_nightmare_sunshine_input_VirtualInputDevices_##name
extern "C" {
JNIEXPORT jlong JNICALL JNI_METHOD(nativeOpen)(JNIEnv*,jclass,jint uid){
  auto o=std::make_unique<Owner>();o->root=getuid()==0; // Caller-provided UID never grants privileges.
  if(uid!=static_cast<int>(getuid())){o->error="Worker UID mismatch";return reinterpret_cast<jlong>(o.release());}
  if(getuid()!=0 && getuid()!=2000){o->error="Kernel input requires a root or Shizuku shell worker";return reinterpret_cast<jlong>(o.release());}
  std::string suffix=std::to_string(getpid());
  if(o->root){o->mouse=uinput(*o,0,"Sunshine Mouse "+suffix);o->keyboard=uinput(*o,1,"Sunshine Keyboard "+suffix);}
  if(!o->mouse)o->mouse=uhid(*o,false,"Sunshine Mouse "+suffix);
  return reinterpret_cast<jlong>(o.release());
}
JNIEXPORT jint JNICALL JNI_METHOD(nativeCapabilities)(JNIEnv*,jclass,jlong p){auto o=owner(p);return o?(o->mouse?1:0)|(o->keyboard?2:0)|(!o->controllers.empty()?4:0):0;}
JNIEXPORT jstring JNICALL JNI_METHOD(nativeLastError)(JNIEnv *e,jclass,jlong p){auto o=owner(p);return e->NewStringUTF(o?o->error.c_str():"Virtual devices closed");}
JNIEXPORT jstring JNICALL JNI_METHOD(nativeDeviceName)(JNIEnv *e,jclass,jlong p,jint kind){auto o=owner(p);if(!o)return nullptr;auto d=kind==1?o->mouse.get():kind==2?o->keyboard.get():nullptr;return d?e->NewStringUTF(d->name.c_str()):nullptr;}
JNIEXPORT jstring JNICALL JNI_METHOD(nativeCreateController)(JNIEnv *e,jclass,jlong p,jint id){
  auto o=owner(p);if(!o)return nullptr;if(id<0||id>=16||o->controllers.count(id)){o->error="Invalid or duplicate controller ID";return nullptr;}
  if(getuid()!=0 && getuid()!=2000){o->error="Kernel input requires a privileged worker";return nullptr;}
  std::string name="Sunshine Gamepad "+std::to_string(getpid())+"-"+std::to_string(++o->serial);
  std::unique_ptr<Device>d;if(o->root)d=uinput(*o,2,name);if(!d)d=uhid(*o,true,name);
  if(!d)return nullptr;
  if(!controller(*o,*d,{}))return nullptr;
  o->controllers.emplace(id,std::move(d));return e->NewStringUTF(name.c_str());
}
JNIEXPORT void JNICALL JNI_METHOD(nativeRemoveController)(JNIEnv*,jclass,jlong p,jint id){auto o=owner(p);if(!o)return;auto it=o->controllers.find(id);if(it!=o->controllers.end()){controller(*o,*it->second,{});o->controllers.erase(it);}}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeMouse)(JNIEnv*,jclass,jlong p,jint x,jint y,jint b,jint v,jint h){auto o=owner(p);if(!o||b<0||b>31)return false;if(mouse(*o,x,y,b,v,h))return true;o->mouse.reset();o->mouseButtons=0;return false;}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeKey)(JNIEnv*,jclass,jlong p,jint code,jboolean release){auto o=owner(p);if(!o||!o->keyboard||code<=0||code>=BTN_MISC)return false;
  if(!event(*o->keyboard,EV_KEY,code,release?0:o->held[code]?2:1,o->error)||!syn(*o->keyboard,o->error)){o->keyboard.reset();return false;}o->held[code]=!release;return true;}
JNIEXPORT jboolean JNICALL JNI_METHOD(nativeController)(JNIEnv*,jclass,jlong p,jint id,jint b,jint lt,jint rt,jint lx,jint ly,jint rx,jint ry){
  auto o=owner(p);if(!o)return false;auto it=o->controllers.find(id);if(it==o->controllers.end()){o->error="Controller unavailable";return false;}
  if(lt<0||lt>255||rt<0||rt>255||lx<-32768||lx>32767||ly<-32768||ly>32767||rx<-32768||rx>32767||ry<-32768||ry>32767){o->error="Invalid controller axis";return false;}
  ControllerState s{static_cast<uint32_t>(b),static_cast<uint8_t>(lt),static_cast<uint8_t>(rt),static_cast<int16_t>(lx),static_cast<int16_t>(ly),static_cast<int16_t>(rx),static_cast<int16_t>(ry)};return controller(*o,*it->second,s);
}
JNIEXPORT void JNICALL JNI_METHOD(nativeReset)(JNIEnv*,jclass,jlong p){if(auto o=owner(p))reset(*o);}
JNIEXPORT void JNICALL JNI_METHOD(nativeClose)(JNIEnv*,jclass,jlong p){if(auto o=owner(p)){reset(*o);delete o;}}
}
