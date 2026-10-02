#include "input_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

namespace {
  constexpr int batch_limit = 32;
  constexpr const char *package = "com/nightmare/sunshine/input/";
  std::recursive_mutex mutex;
  struct cache_t {
    JavaVM *vm {};
    jclass manager {}, event {};
    jmethodID open {}, batch {}, text {}, create {}, remove {}, reset {}, close {}, ctor {};
    jfieldID code {}, failure {}, mask {}, controllers {};
    std::array<jfieldID, 19> integers {};
    std::array<jfieldID, 9> floats {};
    jfieldID release {};
  } cache;
  constexpr const char *integer_names[] = {"kind", "pointerId", "action", "button", "keyCode", "flags", "modifiers", "controllerId", "toolType", "penButtons", "rotation", "tilt", "controllerButtons", "leftTrigger", "rightTrigger", "leftStickX", "leftStickY", "rightStickX", "rightStickY"};
  constexpr const char *float_names[] = {"x", "y", "deltaX", "deltaY", "verticalScroll", "horizontalScroll", "pressure", "contactMajor", "contactMinor"};
  struct attachment {
    JNIEnv *env {};
    bool attached {};
    attachment() {
      if (!cache.vm) return;
      auto result = cache.vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
      if (result == JNI_EDETACHED) attached = cache.vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
      if (result != JNI_OK && !attached) env = nullptr;
    }
    ~attachment() { if (attached) cache.vm->DetachCurrentThread(); }
  };
  bool checked(JNIEnv *env) {
    if (!env->ExceptionCheck()) return true;
    env->ExceptionDescribe();
    env->ExceptionClear();
    BOOST_LOG(error) << "Android input JNI operation failed";
    return false;
  }
  struct event_t {
    std::array<jint, 19> i {};
    std::array<jfloat, 9> f {};
    bool release {};
    event_t() = default;
    explicit event_t(int kind) { i[0] = kind; }
  };
  struct client_t;
  struct global_t {
    std::array<client_t *, platf::MAX_GAMEPADS> owners {};
    std::array<int, platf::MAX_GAMEPADS> controller_ids {};
  };
  thread_local platf::client_input_t *active {};
  std::atomic<uint32_t> next_session {1};
  int allocate_session() {
    auto id = next_session.load();
    while (id <= 0x40000000u) {
      if (next_session.compare_exchange_weak(id, id + 1)) return static_cast<int>(id);
    }
    return 0; // Exhausted IDs cannot be recycled or collide with Java probe sessions.
  }
  struct client_t: platf::client_input_t {
    std::shared_ptr<global_t> global;
    int session {};
    int display = -1, width {}, height {}, rotation {}, mask {}, max_controllers {};
    bool ready {};
    float mouse_x {}, mouse_y {};
    int count {};
    std::array<event_t, batch_limit> staging;
    jobjectArray objects {};
    std::array<jobject, batch_limit> pool {};
    explicit client_t(std::shared_ptr<global_t> g): global(std::move(g)), staging {event_t(0)} {}
    void unmap() {
      for (auto &owner : global->owners) if (owner == this) owner = nullptr;
    }
    void flush() {
      if (!count) return;
      attachment a;
      if (a.env && ready) {
        for (int n = 0; n < count; ++n) {
          auto &e = staging[n];
          for (size_t k = 0; k < e.i.size(); ++k) a.env->SetIntField(pool[n], cache.integers[k], e.i[k]);
          for (size_t k = 0; k < e.f.size(); ++k) a.env->SetFloatField(pool[n], cache.floats[k], e.f[k]);
          a.env->SetBooleanField(pool[n], cache.release, e.release);
        }
        if (checked(a.env)) a.env->CallStaticVoidMethod(cache.manager, cache.batch, session, objects, count);
        if (!checked(a.env)) { ready = false; unmap(); }
      }
      count = 0;
    }
    void enqueue(const event_t &event) {
      if (!ready || !(mask & (1 << (event.i[0] - 1)))) return;
      staging[count++] = event;
      if (count == batch_limit) flush();
      if (active != this) flush();
    }
    void close() {
      flush();
      attachment a;
      if (a.env && display >= 0) {
        a.env->CallStaticVoidMethod(cache.manager, cache.close, session);
        checked(a.env);
      }
      ready = false;
      unmap();
    }
    ~client_t() override {
      std::lock_guard lock(mutex);
      close();
      attachment a;
      if (a.env) {
        for (auto object : pool) if (object) a.env->DeleteGlobalRef(object);
        if (objects) a.env->DeleteGlobalRef(objects);
      }
    }
  };
  client_t *current() { return static_cast<client_t *>(active); }
  bool result_ok(JNIEnv *env, jobject result) {
    bool ok = checked(env) && result && env->GetIntField(result, cache.code) == 0;
    if (result) env->DeleteLocalRef(result);
    return checked(env) && ok;
  }
  bool utf16(const char *text, int size, std::vector<jchar> &out) {
    if (size < 0 || (!text && size)) return false;
    out.reserve(size);
    for (int n = 0; n < size;) {
      auto first = static_cast<unsigned char>(text[n++]);
      uint32_t cp = first;
      int remaining = 0;
      uint32_t minimum = 0;
      if (first >= 0xc2 && first <= 0xdf) { cp &= 0x1f; remaining = 1; minimum = 0x80; }
      else if (first >= 0xe0 && first <= 0xef) { cp &= 0xf; remaining = 2; minimum = 0x800; }
      else if (first >= 0xf0 && first <= 0xf4) { cp &= 7; remaining = 3; minimum = 0x10000; }
      else if (first >= 0x80) return false;
      if (remaining > size - n) return false;
      while (remaining--) {
        auto c = static_cast<unsigned char>(text[n++]);
        if ((c & 0xc0) != 0x80) return false;
        cp = (cp << 6) | (c & 0x3f);
      }
      if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
      if (cp < 0x10000) out.push_back(static_cast<jchar>(cp));
      else { cp -= 0x10000; out.push_back(0xd800 | (cp >> 10)); out.push_back(0xdc00 | (cp & 0x3ff)); }
    }
    return true;
  }
}

namespace platf::android_input {
  bool initialize(JNIEnv *env) {
    std::lock_guard lock(mutex);
    if (cache.vm) return true;
    cache_t c;
    bool lookup_ok = true;
    auto find = [&](const char *name) {
      if (!lookup_ok) return static_cast<jclass>(nullptr);
      auto cls = env->FindClass((std::string(package) + name).c_str());
      lookup_ok = checked(env) && cls;
      return cls;
    };
    jclass manager = find("InputBackendManager"), event = find("InputEventData"), result = find("InputResult"), caps = find("InputCapabilities");
    if (!lookup_ok) {
      for (auto cls : {manager, event, result, caps}) if (cls) env->DeleteLocalRef(cls);
      return false;
    }
    auto method = [&](jclass cls, const char *name, const char *signature, bool is_static = true) {
      if (!lookup_ok) return static_cast<jmethodID>(nullptr);
      auto id = is_static ? env->GetStaticMethodID(cls, name, signature) : env->GetMethodID(cls, name, signature);
      lookup_ok = checked(env) && id;
      return id;
    };
    auto field = [&](jclass cls, const char *name, const char *signature) {
      if (!lookup_ok) return static_cast<jfieldID>(nullptr);
      auto id = env->GetFieldID(cls, name, signature);
      lookup_ok = checked(env) && id;
      return id;
    };
    c.open = method(manager, "openSession", "(IIIII)Lcom/nightmare/sunshine/input/InputCapabilities;");
    c.batch = method(manager, "submitBatch", "(I[Lcom/nightmare/sunshine/input/InputEventData;I)V");
    c.text = method(manager, "submitText", "(ILjava/lang/String;)Lcom/nightmare/sunshine/input/InputResult;");
    c.create = method(manager, "createController", "(II)Lcom/nightmare/sunshine/input/InputResult;");
    c.remove = method(manager, "removeController", "(II)V");
    c.reset = method(manager, "resetSession", "(I)V");
    c.close = method(manager, "closeSession", "(I)V");
    c.ctor = method(event, "<init>", "()V", false);
    c.code = field(result, "code", "I");
    c.failure = field(caps, "failureReason", "Ljava/lang/String;");
    c.mask = field(caps, "supportedEventMask", "I");
    c.controllers = field(caps, "maxControllers", "I");
    for (size_t k = 0; k < c.integers.size(); ++k) c.integers[k] = field(event, integer_names[k], "I");
    for (size_t k = 0; k < c.floats.size(); ++k) c.floats[k] = field(event, float_names[k], "F");
    c.release = field(event, "release", "Z");
    bool ok = lookup_ok;
    if (ok) {
      c.manager = static_cast<jclass>(env->NewGlobalRef(manager));
      ok = checked(env) && c.manager;
      if (ok) {
        c.event = static_cast<jclass>(env->NewGlobalRef(event));
        ok = checked(env) && c.event && env->GetJavaVM(&c.vm) == JNI_OK;
      }
    }
    for (auto cls : {manager, event, result, caps}) env->DeleteLocalRef(cls);
    if (!ok) {
      if (c.manager) env->DeleteGlobalRef(c.manager);
      if (c.event) env->DeleteGlobalRef(c.event);
      return false;
    }
    cache = c;
    return true;
  }
  scope::scope(client_input_t *client): lock_(mutex), previous_(active), client_(client) {
    if (previous_ && previous_ != client) static_cast<client_t *>(previous_)->flush();
    active = client;
  }
  scope::~scope() {
    if (client_) static_cast<client_t *>(client_)->flush();
    active = previous_;
  }
  void configure(client_input_t *client, int displayId, int width, int height, int rotation) {
    std::lock_guard lock(mutex);
    if (!client || displayId < 0 || width <= 0 || height <= 0) return;
    auto &c = *static_cast<client_t *>(client);
    if (c.ready && c.display == displayId && c.width == width && c.height == height && c.rotation == rotation) return;
    attachment a;
    if (!a.env) return;
    c.close();
    c.session = allocate_session();
    if (!c.session) {
      BOOST_LOG(error) << "Android input session ID range exhausted";
      return;
    }
    c.display = displayId; c.width = width; c.height = height; c.rotation = rotation;
    auto caps = a.env->CallStaticObjectMethod(cache.manager, cache.open, c.session, displayId, width, height, rotation);
    if (!checked(a.env) || !caps) return;
    auto failure = static_cast<jstring>(a.env->GetObjectField(caps, cache.failure));
    bool failed = failure && a.env->GetStringLength(failure) != 0;
    c.mask = a.env->GetIntField(caps, cache.mask);
    c.max_controllers = a.env->GetIntField(caps, cache.controllers);
    constexpr int required_mask = 0x3f; // Relative/absolute mouse, buttons, scroll, keys and touch.
    c.ready = checked(a.env) && !failed && (c.mask & required_mask) == required_mask;
    if (failure) a.env->DeleteLocalRef(failure);
    a.env->DeleteLocalRef(caps);
    if (!c.ready) return;
    if (!c.objects) {
      auto array = a.env->NewObjectArray(batch_limit, cache.event, nullptr);
      if (!checked(a.env) || !array) { c.close(); return; }
      c.objects = static_cast<jobjectArray>(a.env->NewGlobalRef(array));
      for (int n = 0; n < batch_limit; ++n) {
        auto object = a.env->NewObject(cache.event, cache.ctor);
        if (!checked(a.env) || !object) break;
        c.pool[n] = a.env->NewGlobalRef(object);
        a.env->SetObjectArrayElement(array, n, object);
        a.env->DeleteLocalRef(object);
        if (!checked(a.env) || !c.pool[n]) break;
      }
      a.env->DeleteLocalRef(array);
      if (!checked(a.env) || !c.objects || !c.pool.back()) {
        c.close();
        for (auto &object : c.pool) {
          if (object) a.env->DeleteGlobalRef(object);
          object = nullptr;
        }
        if (c.objects) a.env->DeleteGlobalRef(c.objects);
        c.objects = nullptr;
      }
    }
  }
  void reset(client_input_t *client) {
    std::lock_guard lock(mutex);
    if (!client) return;
    auto &c = *static_cast<client_t *>(client);
    c.flush();
    attachment a;
    if (a.env && c.ready) {
      a.env->CallStaticVoidMethod(cache.manager, cache.reset, c.session);
      if (!checked(a.env)) c.ready = false;
    }
    c.unmap();
  }
}

namespace platf {
  input_t input() { return input_t(new std::shared_ptr<global_t>(std::make_shared<global_t>())); }
  void freeInput(void *p) { delete static_cast<std::shared_ptr<global_t> *>(p); }
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    if (!input) return {};
    return std::make_unique<client_t>(*static_cast<std::shared_ptr<global_t> *>(input.get()));
  }
  util::point_t get_mouse_loc(input_t &) {
    std::lock_guard lock(mutex);
    auto c = current();
    return {c ? static_cast<double>(c->mouse_x) : 0.0, c ? static_cast<double>(c->mouse_y) : 0.0};
  }
  void move_mouse(input_t &, int x, int y) {
    std::lock_guard lock(mutex);
    if (auto c = current()) {
      event_t e(1); e.f[2] = x; e.f[3] = y;
      c->mouse_x = std::clamp(c->mouse_x + x, 0.0f, static_cast<float>(std::max(0, c->width - 1)));
      c->mouse_y = std::clamp(c->mouse_y + y, 0.0f, static_cast<float>(std::max(0, c->height - 1)));
      c->enqueue(e);
    }
  }
  void abs_mouse(input_t &, const touch_port_t &port, float x, float y) {
    std::lock_guard lock(mutex);
    if (auto c = current()) {
      event_t e(2); e.f[0] = x + port.offset_x; e.f[1] = y + port.offset_y;
      c->mouse_x = e.f[0]; c->mouse_y = e.f[1]; c->enqueue(e);
    }
  }
  void button_mouse(input_t &, int button, bool release) {
    std::lock_guard lock(mutex);
    if (auto c = current()) { event_t e(3); e.i[3] = button; e.release = release; c->enqueue(e); }
  }
  void scroll(input_t &, int distance) {
    std::lock_guard lock(mutex);
    if (auto c = current()) { event_t e(4); e.f[4] = distance; c->enqueue(e); }
  }
  void hscroll(input_t &, int distance) {
    std::lock_guard lock(mutex);
    if (auto c = current()) { event_t e(4); e.f[5] = distance; c->enqueue(e); }
  }
  void keyboard_update(input_t &, uint16_t key, bool release, uint8_t flags, bool extended) {
    std::lock_guard lock(mutex);
    if (auto c = current()) { event_t e(5); e.i[4] = key; e.i[5] = flags; e.i[6] = extended ? 0x10 : 0; e.release = release; c->enqueue(e); }
  }
  void unicode(input_t &, char *text, int size) {
    std::lock_guard lock(mutex);
    auto c = current();
    if (!c || !c->ready) return;
    std::vector<jchar> decoded;
    if (!utf16(text, size, decoded)) { BOOST_LOG(warning) << "Rejected invalid UTF-8 input"; return; }
    c->flush();
    attachment a;
    if (!a.env) return;
    auto string = a.env->NewString(decoded.data(), static_cast<jsize>(decoded.size()));
    if (!checked(a.env) || !string) return;
    auto result = a.env->CallStaticObjectMethod(cache.manager, cache.text, c->session, string);
    if (!result_ok(a.env, result)) BOOST_LOG(warning) << "Android mapped text rejected";
    a.env->DeleteLocalRef(string);
  }
  void touch_update(client_input_t *client, const touch_port_t &port, const touch_input_t &touch) {
    std::lock_guard lock(mutex);
    if (!client) return;
    event_t e(6); e.i[1] = touch.pointerId; e.i[2] = touch.eventType; e.i[10] = touch.rotation;
    e.f[0] = touch.x * port.width + port.offset_x; e.f[1] = touch.y * port.height + port.offset_y;
    e.f[6] = touch.pressureOrDistance; e.f[7] = touch.contactAreaMajor; e.f[8] = touch.contactAreaMinor;
    static_cast<client_t *>(client)->enqueue(e);
  }
  void pen_update(client_input_t *client, const touch_port_t &port, const pen_input_t &pen) {
    std::lock_guard lock(mutex);
    if (!client) return;
    event_t e(7); e.i[2] = pen.eventType; e.i[8] = pen.toolType; e.i[9] = pen.penButtons; e.i[10] = pen.rotation; e.i[11] = pen.tilt;
    e.f[0] = pen.x * port.width + port.offset_x; e.f[1] = pen.y * port.height + port.offset_y;
    e.f[6] = pen.pressureOrDistance; e.f[7] = pen.contactAreaMajor; e.f[8] = pen.contactAreaMinor;
    static_cast<client_t *>(client)->enqueue(e);
  }
  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &, feedback_queue_t) {
    std::lock_guard lock(mutex);
    auto c = current();
    if (!input || !c || !c->ready || id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS || id.clientRelativeIndex >= c->max_controllers || !(c->mask & 0x80)) return -1;
    auto &g = **static_cast<std::shared_ptr<global_t> *>(input.get());
    if (&g != c->global.get() || g.owners[id.globalIndex]) return -1;
    c->flush(); attachment a;
    if (!a.env) return -1;
    auto result = a.env->CallStaticObjectMethod(cache.manager, cache.create, c->session, static_cast<int>(id.clientRelativeIndex));
    if (!result_ok(a.env, result)) return -1;
    g.owners[id.globalIndex] = c; g.controller_ids[id.globalIndex] = id.clientRelativeIndex;
    return 0;
  }
  void free_gamepad(input_t &input, int nr) {
    std::lock_guard lock(mutex);
    if (!input || nr < 0 || nr >= MAX_GAMEPADS) return;
    auto &g = **static_cast<std::shared_ptr<global_t> *>(input.get());
    auto c = g.owners[nr];
    if (!c) return;
    c->flush(); attachment a;
    if (a.env && c->ready) { a.env->CallStaticVoidMethod(cache.manager, cache.remove, c->session, g.controller_ids[nr]); checked(a.env); }
    g.owners[nr] = nullptr;
  }
  void gamepad_update(input_t &input, int nr, const gamepad_state_t &state) {
    std::lock_guard lock(mutex);
    if (!input || nr < 0 || nr >= MAX_GAMEPADS) return;
    auto &g = **static_cast<std::shared_ptr<global_t> *>(input.get());
    auto c = g.owners[nr];
    if (!c || (active && active != c)) return;
    event_t e(8); e.i[7] = g.controller_ids[nr]; e.i[12] = state.buttonFlags; e.i[13] = state.lt; e.i[14] = state.rt;
    e.i[15] = state.lsX; e.i[16] = state.lsY; e.i[17] = state.rsX; e.i[18] = state.rsY; c->enqueue(e);
  }
  // The backend deliberately does not advertise these optional controller features.
  void gamepad_touch(input_t &, const gamepad_touch_t &) {}
  void gamepad_motion(input_t &, const gamepad_motion_t &) {}
  void gamepad_battery(input_t &, const gamepad_battery_t &) {}
  platform_caps::caps_t get_capabilities() { return 0; }
  std::vector<supported_gamepad_t> &supported_gamepads(input_t *) {
    static std::vector<supported_gamepad_t> gamepads {{"auto", true, "Controller availability requires worker device registration"}};
    return gamepads;
  }
}
