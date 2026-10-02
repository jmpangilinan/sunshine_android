#pragma once

#include <jni.h>
#include <mutex>
#include "../common.h"

namespace platf::android_input {
  // Called on a Java-origin thread so class lookup uses the application loader.
  bool initialize(JNIEnv *env);
  class scope {
  public:
    explicit scope(client_input_t *client);
    ~scope();
    scope(const scope &) = delete;
    scope &operator=(const scope &) = delete;
  private:
    std::unique_lock<std::recursive_mutex> lock_;
    client_input_t *previous_;
    client_input_t *client_;
  };
  // No privileged session is opened before valid capture geometry is supplied.
  void configure(client_input_t *client, int displayId, int width, int height, int rotation);
  void reset(client_input_t *client);
}
