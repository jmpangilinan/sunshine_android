/**
 * @file src/input.cpp
 * @brief Definitions for gamepad, keyboard, and mouse input handling.
 */
#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

// standard includes
#include <bitset>
#include <chrono>
#include <cmath>
#include <list>
#include <thread>
#include <unordered_map>

// lib includes
#include <boost/endian/buffers.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "input.h"
#include "input_state.h"
#include "logging.h"
#include "platform/common.h"
#include "platform/android/input_bridge.h"
#include "thread_pool.h"
#include "utility.h"

// Win32 WHEEL_DELTA constant
#ifndef WHEEL_DELTA
  #define WHEEL_DELTA 120
#endif


using namespace std::literals;

namespace input {

  constexpr auto MAX_GAMEPADS = std::min((std::size_t) platf::MAX_GAMEPADS, sizeof(std::int16_t) * 8);
#define DISABLE_LEFT_BUTTON_DELAY ((thread_pool_util::ThreadPool::task_id_t) 0x01)
#define ENABLE_LEFT_BUTTON_DELAY nullptr

  constexpr auto VKEY_SHIFT = 0x10;
  constexpr auto VKEY_LSHIFT = 0xA0;
  constexpr auto VKEY_RSHIFT = 0xA1;
  constexpr auto VKEY_CONTROL = 0x11;
  constexpr auto VKEY_LCONTROL = 0xA2;
  constexpr auto VKEY_RCONTROL = 0xA3;
  constexpr auto VKEY_MENU = 0x12;
  constexpr auto VKEY_LMENU = 0xA4;
  constexpr auto VKEY_RMENU = 0xA5;

  enum class button_state_e {
    NONE,  ///< No button state
    DOWN,  ///< Button is down
    UP  ///< Button is up
  };

  std::mutex gamepad_mask_lock;

  template<std::size_t N>
  int alloc_id(std::bitset<N> &gamepad_mask) {
    std::lock_guard<std::mutex> lock(gamepad_mask_lock);
    for (int x = 0; x < gamepad_mask.size(); ++x) {
      if (!gamepad_mask[x]) {
        gamepad_mask[x] = true;
        return x;
      }
    }

    return -1;
  }

  template<std::size_t N>
  void free_id(std::bitset<N> &gamepad_mask, int id) {
    std::lock_guard<std::mutex> lock(gamepad_mask_lock);
    gamepad_mask[id] = false;
  }

  typedef uint32_t key_press_id_t;

  key_press_id_t make_kpid(uint16_t vk, uint8_t flags) {
    return (key_press_id_t) vk << 8 | flags;
  }

  uint16_t vk_from_kpid(key_press_id_t kpid) {
    return kpid >> 8;
  }

  uint8_t flags_from_kpid(key_press_id_t kpid) {
    return kpid & 0xFF;
  }

  /**
   * @brief Convert a little-endian netfloat to a native endianness float.
   * @param f Netfloat value.
   * @return The native endianness float value.
   */
  float from_netfloat(netfloat f) {
    return boost::endian::endian_load<float, sizeof(float), boost::endian::order::little>(f);
  }

  /**
   * @brief Convert a little-endian netfloat to a native endianness float and clamps it.
   * @param f Netfloat value.
   * @param min The minimium value for clamping.
   * @param max The maximum value for clamping.
   * @return Clamped native endianess float value.
   */
  float from_clamped_netfloat(netfloat f, float min, float max) {
    return std::clamp(from_netfloat(f), min, max);
  }

  using key_press_state_t = state::held_key_state;

  static platf::input_t platf_input;
  static std::bitset<platf::MAX_GAMEPADS> gamepadMask {};

  void free_gamepad(platf::input_t &platf_input, int id) {
    platf::gamepad_update(platf_input, id, platf::gamepad_state_t {});
    platf::free_gamepad(platf_input, id);

    free_id(gamepadMask, id);
  }

  struct gamepad_t {
    gamepad_t():
        gamepad_state {},
        back_timeout_id {},
        id {-1},
        back_button_state {button_state_e::NONE} {
    }

    ~gamepad_t() = default;

    platf::gamepad_state_t gamepad_state;

    thread_pool_util::ThreadPool::task_id_t back_timeout_id;
    thread_pool_util::ThreadPool::task_id_t home_release_id {};

    int id;

    // When emulating the HOME button, we may need to artificially release the back button.
    // Afterwards, the gamepad state on sunshine won't match the state on Moonlight.
    // To prevent Sunshine from sending erroneous input data to the active application,
    // Sunshine forces the button to be in a specific state until the gamepad state matches that of
    // Moonlight once more.
    button_state_e back_button_state;
  };

  struct input_t {
    enum shortkey_e {
      CTRL = 0x1,  ///< Control key
      ALT = 0x2,  ///< Alt key
      SHIFT = 0x4,  ///< Shift key
      SHORTCUT = CTRL | ALT | SHIFT  ///< Shortcut combination
    };

    input_t(
      safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event,
      platf::feedback_queue_t feedback_queue
    ):
        shortcutFlags {},
        gamepads(MAX_GAMEPADS),
        client_context {platf::allocate_client_input_context(platf_input)},
        touch_port_event {std::move(touch_port_event)},
        feedback_queue {std::move(feedback_queue)},
        mouse_left_button_timeout {},
        touch_port {{0, 0, 0, 0}, 0, 0, 1.0f},
        accumulated_vscroll_delta {},
        accumulated_hscroll_delta {} {
    }
    ~input_t() {
      task_pool.cancel(key_press_repeat_id);
      task_pool.cancel(mouse_left_button_timeout);
      for (auto &gamepad : gamepads) {
        task_pool.cancel(gamepad.back_timeout_id);
        task_pool.cancel(gamepad.home_release_id);
        if (gamepad.id >= 0) free_gamepad(platf_input, gamepad.id);
      }
    }

    std::recursive_mutex dispatch_lock;
    task_pool_util::TaskPool::task_id_t key_press_repeat_id {};
    std::unordered_map<key_press_id_t, key_press_state_t> key_press;
    std::array<uint8_t, 6> mouse_press {};

    // Keep track of alt+ctrl+shift key combo
    int shortcutFlags;

    std::vector<gamepad_t> gamepads;
    std::unique_ptr<platf::client_input_t> client_context;

    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event;
    platf::feedback_queue_t feedback_queue;

    state::ordered_queue input_queue;

    thread_pool_util::ThreadPool::task_id_t mouse_left_button_timeout;

    input::touch_port_t touch_port;

    int32_t accumulated_vscroll_delta;
    int32_t accumulated_hscroll_delta;
  };

  /**
   * @brief Apply shortcut based on VKEY
   * @param keyCode The VKEY code
   * @return 0 if no shortcut applied, > 0 if shortcut applied.
   */
  inline int apply_shortcut(short keyCode) {
    constexpr auto VK_F1 = 0x70;
    constexpr auto VK_F13 = 0x7C;

    BOOST_LOG(debug) << "Apply Shortcut: 0x"sv << util::hex((std::uint8_t) keyCode).to_string_view();

    if (keyCode >= VK_F1 && keyCode <= VK_F13) {
      mail::man->event<int>(mail::switch_display)->raise(keyCode - VK_F1);
      return 1;
    }

    switch (keyCode) {
      case 0x4E /* VKEY_N */:
        display_cursor = !display_cursor;
        return 1;
    }

    return 0;
  }

  void print(PNV_REL_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin relative mouse move packet--"sv << std::endl
      << "deltaX ["sv << util::endian::big(packet->deltaX) << ']' << std::endl
      << "deltaY ["sv << util::endian::big(packet->deltaY) << ']' << std::endl
      << "--end relative mouse move packet--"sv;
  }

  void print(PNV_ABS_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin absolute mouse move packet--"sv << std::endl
      << "x      ["sv << util::endian::big(packet->x) << ']' << std::endl
      << "y      ["sv << util::endian::big(packet->y) << ']' << std::endl
      << "width  ["sv << util::endian::big(packet->width) << ']' << std::endl
      << "height ["sv << util::endian::big(packet->height) << ']' << std::endl
      << "--end absolute mouse move packet--"sv;
  }

  void print(PNV_MOUSE_BUTTON_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse button packet--"sv << std::endl
      << "action ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "button ["sv << util::hex(packet->button).to_string_view() << ']' << std::endl
      << "--end mouse button packet--"sv;
  }

  void print(PNV_SCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse scroll packet--"sv << std::endl
      << "scrollAmt1 ["sv << util::endian::big(packet->scrollAmt1) << ']' << std::endl
      << "--end mouse scroll packet--"sv;
  }

  void print(PSS_HSCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse hscroll packet--"sv << std::endl
      << "scrollAmount ["sv << util::endian::big(packet->scrollAmount) << ']' << std::endl
      << "--end mouse hscroll packet--"sv;
  }

  void print(PNV_KEYBOARD_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin keyboard packet--"sv << std::endl
      << "keyAction ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "keyCode ["sv << util::hex(packet->keyCode).to_string_view() << ']' << std::endl
      << "modifiers ["sv << util::hex(packet->modifiers).to_string_view() << ']' << std::endl
      << "flags ["sv << util::hex(packet->flags).to_string_view() << ']' << std::endl
      << "--end keyboard packet--"sv;
  }

  void print(PNV_UNICODE_PACKET packet) {
    std::string text(packet->text, util::endian::big(packet->header.size) - sizeof(packet->header.magic));
    BOOST_LOG(debug)
      << "--begin unicode packet--"sv << std::endl
      << "text ["sv << text << ']' << std::endl
      << "--end unicode packet--"sv;
  }

  void print(PNV_MULTI_CONTROLLER_PACKET packet) {
    // Moonlight spams controller packet even when not necessary
    BOOST_LOG(verbose)
      << "--begin controller packet--"sv << std::endl
      << "controllerNumber ["sv << packet->controllerNumber << ']' << std::endl
      << "activeGamepadMask ["sv << util::hex(packet->activeGamepadMask).to_string_view() << ']' << std::endl
      << "buttonFlags ["sv << util::hex((uint32_t) packet->buttonFlags | (packet->buttonFlags2 << 16)).to_string_view() << ']' << std::endl
      << "leftTrigger ["sv << util::hex(packet->leftTrigger).to_string_view() << ']' << std::endl
      << "rightTrigger ["sv << util::hex(packet->rightTrigger).to_string_view() << ']' << std::endl
      << "leftStickX ["sv << packet->leftStickX << ']' << std::endl
      << "leftStickY ["sv << packet->leftStickY << ']' << std::endl
      << "rightStickX ["sv << packet->rightStickX << ']' << std::endl
      << "rightStickY ["sv << packet->rightStickY << ']' << std::endl
      << "--end controller packet--"sv;
  }

  /**
   * @brief Prints a touch packet.
   * @param packet The touch packet.
   */
  void print(PSS_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin touch packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "--end touch packet--"sv;
  }

  /**
   * @brief Prints a pen packet.
   * @param packet The pen packet.
   */
  void print(PSS_PEN_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin pen packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "toolType ["sv << util::hex(packet->toolType).to_string_view() << ']' << std::endl
      << "penButtons ["sv << util::hex(packet->penButtons).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "tilt ["sv << (uint32_t) packet->tilt << ']' << std::endl
      << "--end pen packet--"sv;
  }

  /**
   * @brief Prints a controller arrival packet.
   * @param packet The controller arrival packet.
   */
  void print(PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller arrival packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "type ["sv << util::hex(packet->type).to_string_view() << ']' << std::endl
      << "capabilities ["sv << util::hex(packet->capabilities).to_string_view() << ']' << std::endl
      << "supportedButtonFlags ["sv << util::hex(packet->supportedButtonFlags).to_string_view() << ']' << std::endl
      << "--end controller arrival packet--"sv;
  }

  /**
   * @brief Prints a controller touch packet.
   * @param packet The controller touch packet.
   */
  void print(PSS_CONTROLLER_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller touch packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressure ["sv << from_netfloat(packet->pressure) << ']' << std::endl
      << "--end controller touch packet--"sv;
  }

  /**
   * @brief Prints a controller motion packet.
   * @param packet The controller motion packet.
   */
  void print(PSS_CONTROLLER_MOTION_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller motion packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "motionType ["sv << util::hex(packet->motionType).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "z ["sv << from_netfloat(packet->z) << ']' << std::endl
      << "--end controller motion packet--"sv;
  }

  /**
   * @brief Prints a controller battery packet.
   * @param packet The controller battery packet.
   */
  void print(PSS_CONTROLLER_BATTERY_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller battery packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "batteryState ["sv << util::hex(packet->batteryState).to_string_view() << ']' << std::endl
      << "batteryPercentage ["sv << util::hex(packet->batteryPercentage).to_string_view() << ']' << std::endl
      << "--end controller battery packet--"sv;
  }

  void print(void *payload) {
    auto header = (PNV_INPUT_HEADER) payload;

    switch (util::endian::little(header->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        print((PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        print((PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        print((PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        print((PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        print((PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        print((PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        print((PNV_UNICODE_PACKET) payload);
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        print((PNV_MULTI_CONTROLLER_PACKET) payload);
        break;
      case SS_TOUCH_MAGIC:
        print((PSS_TOUCH_PACKET) payload);
        break;
      case SS_PEN_MAGIC:
        print((PSS_PEN_PACKET) payload);
        break;
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        print((PSS_CONTROLLER_ARRIVAL_PACKET) payload);
        break;
      case SS_CONTROLLER_TOUCH_MAGIC:
        print((PSS_CONTROLLER_TOUCH_PACKET) payload);
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        print((PSS_CONTROLLER_MOTION_PACKET) payload);
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        print((PSS_CONTROLLER_BATTERY_PACKET) payload);
        break;
    }
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_REL_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    input->mouse_left_button_timeout = DISABLE_LEFT_BUTTON_DELAY;
    platf::move_mouse(platf_input, util::endian::big(packet->deltaX), util::endian::big(packet->deltaY));
  }

  /**
   * @brief Converts client coordinates on the specified surface into screen coordinates.
   * @param input The input context.
   * @param val The cartesian coordinate pair to convert.
   * @param size The size of the client's surface containing the value.
   * @return The host-relative coordinate pair if a touchport is available.
   */
  std::optional<std::pair<float, float>> client_to_touchport(std::shared_ptr<input_t> &input, const std::pair<float, float> &val, const std::pair<float, float> &size) {
    auto &touch_port_event = input->touch_port_event;
    auto &touch_port = input->touch_port;
    if (touch_port_event->peek()) {
      touch_port = *touch_port_event->pop();
    }
    if (!touch_port) {
      BOOST_LOG(verbose) << "Ignoring early absolute input without a touch port"sv;
      return std::nullopt;
    }

    auto scalarX = touch_port.width / size.first;
    auto scalarY = touch_port.height / size.second;

    float x = std::clamp(val.first, 0.0f, size.first) * scalarX;
    float y = std::clamp(val.second, 0.0f, size.second) * scalarY;

    auto offsetX = touch_port.client_offsetX;
    auto offsetY = touch_port.client_offsetY;

    x = std::clamp(x, offsetX, (size.first * scalarX) - offsetX);
    y = std::clamp(y, offsetY, (size.second * scalarY) - offsetY);

    return std::pair {(x - offsetX) * touch_port.scalar_inv, (y - offsetY) * touch_port.scalar_inv};
  }

  /**
   * @brief Multiply a polar coordinate pair by a cartesian scaling factor.
   * @param r The radial coordinate.
   * @param angle The angular coordinate (radians).
   * @param scalar The scalar cartesian coordinate pair.
   * @return The scaled radial coordinate.
   */
  float multiply_polar_by_cartesian_scalar(float r, float angle, const std::pair<float, float> &scalar) {
    // Convert polar to cartesian coordinates
    float x = r * std::cos(angle);
    float y = r * std::sin(angle);

    // Scale the values
    x *= scalar.first;
    y *= scalar.second;

    // Convert the result back to a polar radial coordinate
    return std::sqrt(std::pow(x, 2) + std::pow(y, 2));
  }

  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar) {
    // If the rotation is unknown, we'll just scale both axes equally by using
    // a 45-degree angle for our scaling calculations
    float angle = rotation == LI_ROT_UNKNOWN ? (M_PI / 4) : (rotation * (M_PI / 180));

    // If we have a major but not a minor axis, treat the touch as circular
    float major = val.first;
    float minor = val.second != 0.0f ? val.second : val.first;

    // The minor axis is perpendicular to major axis so the angle must be rotated by 90 degrees
    return {multiply_polar_by_cartesian_scalar(major, angle, scalar), multiply_polar_by_cartesian_scalar(minor, angle + (M_PI / 2), scalar)};
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_ABS_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (input->mouse_left_button_timeout == DISABLE_LEFT_BUTTON_DELAY) {
      input->mouse_left_button_timeout = ENABLE_LEFT_BUTTON_DELAY;
    }

    float x = util::endian::big(packet->x);
    float y = util::endian::big(packet->y);

    // Prevent divide by zero
    // Don't expect it to happen, but just in case
    if (!packet->width || !packet->height) {
      BOOST_LOG(warning) << "Moonlight passed invalid dimensions"sv;

      return;
    }

    auto width = (float) util::endian::big(packet->width);
    auto height = (float) util::endian::big(packet->height);

    auto tpcoords = client_to_touchport(input, {x, y}, {width, height});
    if (!tpcoords) {
      return;
    }

    auto &touch_port = input->touch_port;
    platf::touch_port_t abs_port {
      touch_port.offset_x,
      touch_port.offset_y,
      touch_port.env_width,
      touch_port.env_height
    };

    platf::abs_mouse(platf_input, abs_port, tpcoords->first, tpcoords->second);
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5;
    auto button = util::endian::big(packet->button);
    if (button > 0 && button < input->mouse_press.size()) {
      if (input->mouse_press[button] != release) {
        return;
      }

      input->mouse_press[button] = !release;
    }
    /**
     * When Moonlight sends mouse input through absolute coordinates,
     * it's possible that BUTTON_RIGHT is pressed down immediately after releasing BUTTON_LEFT.
     * As a result, Sunshine will left-click on hyperlinks in the browser before right-clicking
     *
     * This can be solved by delaying BUTTON_LEFT, however, any delay on input is undesirable during gaming
     * As a compromise, Sunshine will only put delays on BUTTON_LEFT when
     * absolute mouse coordinates have been sent.
     *
     * Try to make sure BUTTON_RIGHT gets called before BUTTON_LEFT is released.
     *
     * input->mouse_left_button_timeout can only be nullptr
     * when the last mouse coordinates were absolute
     */
    if (button == BUTTON_LEFT && release && !input->mouse_left_button_timeout) {
      auto f = [input, release, generation = input->input_queue.generation()]() {
        std::lock_guard<std::recursive_mutex> lock(input->dispatch_lock);
        if (!input->input_queue.valid(generation)) return;
        platf::android_input::scope scope(input->client_context.get());
        auto left_released = input->mouse_press[BUTTON_LEFT];
        if (left_released) {
          // Already released left button
          return;
        }
        platf::button_mouse(platf_input, BUTTON_LEFT, release);

        input->mouse_press[BUTTON_LEFT] = false;
        input->mouse_left_button_timeout = nullptr;
      };

      input->mouse_left_button_timeout = task_pool.pushDelayed(std::move(f), 10ms).task_id;

      return;
    }
    if (
      button == BUTTON_RIGHT && !release &&
      input->mouse_left_button_timeout > DISABLE_LEFT_BUTTON_DELAY
    ) {
      platf::button_mouse(platf_input, BUTTON_RIGHT, false);
      platf::button_mouse(platf_input, BUTTON_RIGHT, true);

      input->mouse_press[BUTTON_RIGHT] = false;

      return;
    }

    platf::button_mouse(platf_input, button, release);
  }

  short map_keycode(short keycode) {
    auto it = config::input.keybindings.find(keycode);
    if (it != std::end(config::input.keybindings)) {
      return it->second;
    }

    return keycode;
  }

  /**
   * @brief Update flags for keyboard shortcut combo's
   */
  inline void update_shortcutFlags(int *flags, short keyCode, bool release) {
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
        if (release) {
          *flags &= ~input_t::SHIFT;
        } else {
          *flags |= input_t::SHIFT;
        }
        break;
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
        if (release) {
          *flags &= ~input_t::CTRL;
        } else {
          *flags |= input_t::CTRL;
        }
        break;
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        if (release) {
          *flags &= ~input_t::ALT;
        } else {
          *flags |= input_t::ALT;
        }
        break;
    }
  }

  bool is_modifier(uint16_t keyCode) {
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        return true;
      default:
        return false;
    }
  }

  void send_key_and_modifiers(uint16_t key_code, bool release, uint8_t flags, uint8_t synthetic_modifiers, bool extended = false) {
    if (!release) {
      // Press any synthetic modifiers required for this key
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        platf::keyboard_update(platf_input, VKEY_SHIFT, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        platf::keyboard_update(platf_input, VKEY_CONTROL, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        platf::keyboard_update(platf_input, VKEY_MENU, false, flags);
      }
    }

    platf::keyboard_update(platf_input, map_keycode(key_code), release, flags, extended);

    if (!release) {
      // Raise any synthetic modifier keys we pressed
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        platf::keyboard_update(platf_input, VKEY_SHIFT, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        platf::keyboard_update(platf_input, VKEY_CONTROL, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        platf::keyboard_update(platf_input, VKEY_MENU, true, flags);
      }
    }
  }

  void repeat_key(std::shared_ptr<input_t> input, uint64_t generation, uint16_t key_code, uint8_t flags, uint8_t synthetic_modifiers) {
    std::lock_guard<std::recursive_mutex> lock(input->dispatch_lock);
    if (!input->input_queue.valid(generation)) return;
    auto &state = input->key_press[make_kpid(key_code, flags)];
    if (!state.pressed) {
      input->key_press_repeat_id = nullptr;
      return;
    }
    platf::android_input::scope scope(input->client_context.get());
    send_key_and_modifiers(key_code, false, flags, synthetic_modifiers, state.extended);
    input->key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_period, input, generation, key_code, flags, synthetic_modifiers).task_id;
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == KEY_UP_EVENT_MAGIC;
    auto keyCode = packet->keyCode & 0x00FF;

    // Set synthetic modifier flags if the keyboard packet is requesting modifier
    // keys that are not current pressed.
    uint8_t synthetic_modifiers = 0;
    if (!release && !is_modifier(keyCode)) {
      if (!(input->shortcutFlags & input_t::SHIFT) && (packet->modifiers & MODIFIER_SHIFT)) {
        synthetic_modifiers |= MODIFIER_SHIFT;
      }
      if (!(input->shortcutFlags & input_t::CTRL) && (packet->modifiers & MODIFIER_CTRL)) {
        synthetic_modifiers |= MODIFIER_CTRL;
      }
      if (!(input->shortcutFlags & input_t::ALT) && (packet->modifiers & MODIFIER_ALT)) {
        synthetic_modifiers |= MODIFIER_ALT;
      }
    }

    auto &state = input->key_press[make_kpid(keyCode, packet->flags)];
    auto &pressed = state.pressed;
    if (!pressed) {
      if (!release) {
        // A new key has been pressed down, we need to check for key combo's
        // If a key-combo has been pressed down, don't pass it through
        if (input->shortcutFlags == input_t::SHORTCUT && apply_shortcut(keyCode) > 0) {
          return;
        }

        if (input->key_press_repeat_id) {
          task_pool.cancel(input->key_press_repeat_id);
        }

        if (config::input.key_repeat_delay.count() > 0) {
          input->key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_delay, input, input->input_queue.generation(), keyCode, packet->flags, synthetic_modifiers).task_id;
        }
      } else {
        // Already released
        return;
      }
    } else if (!release) {
      // Already pressed down key
      return;
    }

    state.update(!release, (packet->modifiers & MODIFIER_EXTENDED) != 0);

    send_key_and_modifiers(keyCode, release, packet->flags, synthetic_modifiers, state.extended);

    update_shortcutFlags(&input->shortcutFlags, keyCode, release);
  }

  /**
   * @brief Called to pass a vertical scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_SCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling) {
      platf::scroll(platf_input, util::endian::big(packet->scrollAmt1));
    } else {
      input->accumulated_vscroll_delta += util::endian::big(packet->scrollAmt1);
      auto full_ticks = input->accumulated_vscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::scroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_vscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  /**
   * @brief Called to pass a horizontal scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_HSCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    if (config::input.high_resolution_scrolling) {
      platf::hscroll(platf_input, util::endian::big(packet->scrollAmount));
    } else {
      input->accumulated_hscroll_delta += util::endian::big(packet->scrollAmount);
      auto full_ticks = input->accumulated_hscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::hscroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_hscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  void passthrough(PNV_UNICODE_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    auto size = util::endian::big(packet->header.size) - sizeof(packet->header.magic);
    platf::unicode(platf_input, packet->text, size);
  }

  /**
   * @brief Called to pass a controller arrival message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller arrival packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    if (input->gamepads[packet->controllerNumber].id >= 0) {
      BOOST_LOG(warning) << "ControllerNumber already allocated ["sv << packet->controllerNumber << ']';
      return;
    }

    platf::gamepad_arrival_t arrival {
      packet->type,
      util::endian::little(packet->capabilities),
      util::endian::little(packet->supportedButtonFlags),
    };

    auto id = alloc_id(gamepadMask);
    if (id < 0) {
      return;
    }

    // Allocate a new gamepad
    if (platf::alloc_gamepad(platf_input, {id, packet->controllerNumber}, arrival, input->feedback_queue)) {
      free_id(gamepadMask, id);
      return;
    }

    input->gamepads[packet->controllerNumber].id = id;
  }

  /**
   * @brief Called to pass a touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The touch packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_TOUCH_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto coords = client_to_touchport(input, {from_clamped_netfloat(packet->x, 0.f, 1.f) * 65535.f, from_clamped_netfloat(packet->y, 0.f, 1.f) * 65535.f}, {65535.f, 65535.f});
    if (!coords) return;
    auto &port = input->touch_port;
    platf::touch_port_t abs_port {port.offset_x, port.offset_y, port.env_width, port.env_height};
    auto rotation = util::endian::little(packet->rotation);
    if (rotation != LI_ROT_UNKNOWN) rotation %= 360;
    auto contact = scale_client_contact_area(
      {from_clamped_netfloat(packet->contactAreaMajor, 0.f, 1.f) * 65535.f,
       from_clamped_netfloat(packet->contactAreaMinor, 0.f, 1.f) * 65535.f},
      rotation, {abs_port.width / 65535.f, abs_port.height / 65535.f});
    platf::touch_input_t touch {packet->eventType, rotation, util::endian::little(packet->pointerId),
      coords->first / abs_port.width, coords->second / abs_port.height,
      from_clamped_netfloat(packet->pressureOrDistance, 0.f, 1.f), contact.first, contact.second};
    platf::touch_update(input->client_context.get(), abs_port, touch);
  }

  /**
   * @brief Called to pass a pen message to the platform backend.
   * @param input The input context pointer.
   * @param packet The pen packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_PEN_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    // Convert the client normalized coordinates to touchport coordinates
    auto coords = client_to_touchport(input, {from_clamped_netfloat(packet->x, 0.0f, 1.0f) * 65535.f, from_clamped_netfloat(packet->y, 0.0f, 1.0f) * 65535.f}, {65535.f, 65535.f});
    if (!coords) {
      return;
    }

    auto &touch_port = input->touch_port;
    platf::touch_port_t abs_port {
      touch_port.offset_x,
      touch_port.offset_y,
      touch_port.env_width,
      touch_port.env_height
    };

    // Renormalize the coordinates
    coords->first /= abs_port.width;
    coords->second /= abs_port.height;

    // Normalize rotation value to 0-359 degree range
    auto rotation = util::endian::little(packet->rotation);
    if (rotation != LI_ROT_UNKNOWN) {
      rotation %= 360;
    }

    // Normalize the contact area based on the touchport
    auto contact_area = scale_client_contact_area(
      {from_clamped_netfloat(packet->contactAreaMajor, 0.0f, 1.0f) * 65535.f,
       from_clamped_netfloat(packet->contactAreaMinor, 0.0f, 1.0f) * 65535.f},
      rotation,
      {abs_port.width / 65535.f, abs_port.height / 65535.f}
    );

    platf::pen_input_t pen {
      packet->eventType,
      packet->toolType,
      packet->penButtons,
      packet->tilt,
      rotation,
      coords->first,
      coords->second,
      from_clamped_netfloat(packet->pressureOrDistance, 0.0f, 1.0f),
      contact_area.first,
      contact_area.second,
    };

    platf::pen_update(input->client_context.get(), abs_port, pen);
  }

  /**
   * @brief Called to pass a controller touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller touch packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_TOUCH_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_touch_t touch {
      {gamepad.id, packet->controllerNumber},
      packet->eventType,
      util::endian::little(packet->pointerId),
      from_clamped_netfloat(packet->x, 0.0f, 1.0f),
      from_clamped_netfloat(packet->y, 0.0f, 1.0f),
      from_clamped_netfloat(packet->pressure, 0.0f, 1.0f),
    };

    platf::gamepad_touch(platf_input, touch);
  }

  /**
   * @brief Called to pass a controller motion message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller motion packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_MOTION_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_motion_t motion {
      {gamepad.id, packet->controllerNumber},
      packet->motionType,
      from_netfloat(packet->x),
      from_netfloat(packet->y),
      from_netfloat(packet->z),
    };

    platf::gamepad_motion(platf_input, motion);
  }

  /**
   * @brief Called to pass a controller battery message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller battery packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_BATTERY_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_battery_t battery {
      {gamepad.id, packet->controllerNumber},
      packet->batteryState,
      packet->batteryPercentage
    };

    platf::gamepad_battery(platf_input, battery);
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (packet->controllerNumber < 0 || packet->controllerNumber >= input->gamepads.size()) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';

      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];

    // If this is an event for a new gamepad, create the gamepad now. Ideally, the client would
    // send a controller arrival instead of this but it's still supported for legacy clients.
    if ((packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id < 0) {
      auto id = alloc_id(gamepadMask);
      if (id < 0) {
        return;
      }

      if (platf::alloc_gamepad(platf_input, {id, (uint8_t) packet->controllerNumber}, {}, input->feedback_queue)) {
        free_id(gamepadMask, id);
        return;
      }

      gamepad.id = id;
    } else if (!(packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id >= 0) {
      // If this is the final event for a gamepad being removed, free the gamepad and return.
      task_pool.cancel(gamepad.back_timeout_id);
      task_pool.cancel(gamepad.home_release_id);
      gamepad.home_release_id = nullptr;
      gamepad.back_timeout_id = nullptr;
      free_gamepad(platf_input, gamepad.id);
      gamepad.id = -1;
      return;
    }

    // If this gamepad has not been initialized, ignore it.
    // This could happen when platf::alloc_gamepad fails
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    std::uint16_t bf = packet->buttonFlags;
    std::uint32_t bf2 = packet->buttonFlags2;
    platf::gamepad_state_t gamepad_state {
      bf | (bf2 << 16),
      packet->leftTrigger,
      packet->rightTrigger,
      packet->leftStickX,
      packet->leftStickY,
      packet->rightStickX,
      packet->rightStickY
    };

    auto bf_new = gamepad_state.buttonFlags;
    switch (gamepad.back_button_state) {
      case button_state_e::UP:
        if (!(platf::BACK & bf_new)) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags &= ~platf::BACK;
        break;
      case button_state_e::DOWN:
        if (platf::BACK & bf_new) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags |= platf::BACK;
        break;
      case button_state_e::NONE:
        break;
    }

    bf = gamepad_state.buttonFlags ^ gamepad.gamepad_state.buttonFlags;
    bf_new = gamepad_state.buttonFlags;

    if (platf::BACK & bf) {
      if (platf::BACK & bf_new) {
        // Don't emulate home button if timeout < 0
        if (config::input.back_button_timeout >= 0ms) {
          auto f = [input, controller = packet->controllerNumber, id = gamepad.id, generation = input->input_queue.generation()]() {
            std::lock_guard<std::recursive_mutex> lock(input->dispatch_lock);
            if (!input->input_queue.valid(generation)) return;
            platf::android_input::scope scope(input->client_context.get());
            auto &gamepad = input->gamepads[controller];
            if (gamepad.id != id || !gamepad.back_timeout_id) return;

            auto &state = gamepad.gamepad_state;

            // Force the back button up
            gamepad.back_button_state = button_state_e::UP;
            state.buttonFlags &= ~platf::BACK;
            platf::gamepad_update(platf_input, gamepad.id, state);

            // Press Home button
            state.buttonFlags |= platf::HOME;
            platf::gamepad_update(platf_input, gamepad.id, state);

            gamepad.back_timeout_id = nullptr;
            gamepad.home_release_id = task_pool.pushDelayed([input, controller, id, generation]() {
              std::lock_guard<std::recursive_mutex> lock(input->dispatch_lock);
              if (!input->input_queue.valid(generation)) return;
              auto &gamepad = input->gamepads[controller];
              if (gamepad.id != id || !gamepad.home_release_id) return;
              platf::android_input::scope scope(input->client_context.get());
              gamepad.gamepad_state.buttonFlags &= ~platf::HOME;
              platf::gamepad_update(platf_input, id, gamepad.gamepad_state);
              gamepad.home_release_id = nullptr;
            }, 100ms).task_id;
          };

          gamepad.back_timeout_id = task_pool.pushDelayed(std::move(f), config::input.back_button_timeout).task_id;
        }
      } else if (gamepad.back_timeout_id) {
        task_pool.cancel(gamepad.back_timeout_id);
        gamepad.back_timeout_id = nullptr;
      }
    }

    platf::gamepad_update(platf_input, gamepad.id, gamepad_state);

    gamepad.gamepad_state = gamepad_state;
  }

  enum class batch_result_e {
    batched,  ///< This entry was batched with the source entry
    not_batchable,  ///< Not eligible to batch but continue attempts to batch
    terminate_batch,  ///< Stop trying to batch with this entry
  };

  /**
   * @brief Batch two relative mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src) {
    short deltaX, deltaY;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->deltaX), util::endian::big(src->deltaX), &deltaX)) {
      return batch_result_e::terminate_batch;
    }
    if (!__builtin_add_overflow(util::endian::big(dest->deltaY), util::endian::big(src->deltaY), &deltaY)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of deltas
    dest->deltaX = util::endian::big(deltaX);
    dest->deltaY = util::endian::big(deltaY);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two absolute mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src) {
    // Batching must only happen if the reference width and height don't change
    if (dest->width != src->width || dest->height != src->height) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest absolute position
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two vertical scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->scrollAmt1), util::endian::big(src->scrollAmt1), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmt1 = util::endian::big(scrollAmt);
    dest->scrollAmt2 = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two horizontal scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (!__builtin_add_overflow(util::endian::big(dest->scrollAmount), util::endian::big(src->scrollAmount), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmount = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller state messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_MULTI_CONTROLLER_PACKET dest, PNV_MULTI_CONTROLLER_PACKET src) {
    // Do not allow batching if the active controllers change
    if (dest->activeGamepadMask != src->activeGamepadMask) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Do not allow batching if the button state changes on this controller
    if (dest->buttonFlags != src->buttonFlags || dest->buttonFlags2 != src->buttonFlags2) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_TOUCH_PACKET dest, PSS_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE &&
        src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two pen messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_PEN_PACKET dest, PSS_PEN_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same type
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Do not allow batching if the button state changes
    if (dest->penButtons != src->penButtons) {
      return batch_result_e::terminate_batch;
    }

    // Do not batch beyond tool changes
    if (dest->toolType != src->toolType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_TOUCH_PACKET dest, PSS_CONTROLLER_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE &&
        dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE &&
        src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller motion messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_MOTION_PACKET dest, PSS_CONTROLLER_MOTION_PACKET src) {
    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Batched events must be the same sensor
    if (dest->motionType != src->motionType) {
      return batch_result_e::not_batchable;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two input messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_INPUT_HEADER dest, PNV_INPUT_HEADER src) {
    // We can only batch if the packet types are the same
    if (dest->magic != src->magic) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch certain message types
    switch (util::endian::little(dest->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return batch((PNV_REL_MOUSE_MOVE_PACKET) dest, (PNV_REL_MOUSE_MOVE_PACKET) src);
      case MOUSE_MOVE_ABS_MAGIC:
        return batch((PNV_ABS_MOUSE_MOVE_PACKET) dest, (PNV_ABS_MOUSE_MOVE_PACKET) src);
      case SCROLL_MAGIC_GEN5:
        return batch((PNV_SCROLL_PACKET) dest, (PNV_SCROLL_PACKET) src);
      case SS_HSCROLL_MAGIC:
        return batch((PSS_HSCROLL_PACKET) dest, (PSS_HSCROLL_PACKET) src);
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return batch((PNV_MULTI_CONTROLLER_PACKET) dest, (PNV_MULTI_CONTROLLER_PACKET) src);
      case SS_TOUCH_MAGIC:
        return batch((PSS_TOUCH_PACKET) dest, (PSS_TOUCH_PACKET) src);
      case SS_PEN_MAGIC:
        return batch((PSS_PEN_PACKET) dest, (PSS_PEN_PACKET) src);
      case SS_CONTROLLER_TOUCH_MAGIC:
        return batch((PSS_CONTROLLER_TOUCH_PACKET) dest, (PSS_CONTROLLER_TOUCH_PACKET) src);
      case SS_CONTROLLER_MOTION_MAGIC:
        return batch((PSS_CONTROLLER_MOTION_PACKET) dest, (PSS_CONTROLLER_MOTION_PACKET) src);
      default:
        // Not a batchable message type
        return batch_result_e::terminate_batch;
    }
  }

  /**
   * @brief Called on a thread pool thread to process an input message.
   * @param input The input context pointer.
   */
  void passthrough_next_message(std::shared_ptr<input_t> input) {
    std::lock_guard<std::recursive_mutex> dispatch(input->dispatch_lock);
    platf::android_input::scope scope(input->client_context.get());
    for (unsigned processed = 0; processed < 32; ++processed) {
    // 'entry' backs the 'payload' pointer, so they must remain in scope together
    std::vector<uint8_t> entry;
    if (!input->input_queue.take(entry, [](auto &dest, auto &src) {
      return static_cast<int>(batch(reinterpret_cast<PNV_INPUT_HEADER>(dest.data()), reinterpret_cast<PNV_INPUT_HEADER>(src.data())));
    })) break;
    auto payload = reinterpret_cast<PNV_INPUT_HEADER>(entry.data());


    // Send the batched input to the OS
    switch (util::endian::little(payload->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        passthrough(input, (PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        passthrough(input, (PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        passthrough(input, (PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        passthrough(input, (PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        passthrough(input, (PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        passthrough(input, (PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        passthrough((PNV_UNICODE_PACKET) payload);
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        passthrough(input, (PNV_MULTI_CONTROLLER_PACKET) payload);
        break;
      case SS_TOUCH_MAGIC:
        passthrough(input, (PSS_TOUCH_PACKET) payload);
        break;
      case SS_PEN_MAGIC:
        passthrough(input, (PSS_PEN_PACKET) payload);
        break;
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        passthrough(input, (PSS_CONTROLLER_ARRIVAL_PACKET) payload);
        break;
      case SS_CONTROLLER_TOUCH_MAGIC:
        passthrough(input, (PSS_CONTROLLER_TOUCH_PACKET) payload);
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        passthrough(input, (PSS_CONTROLLER_MOTION_PACKET) payload);
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        passthrough(input, (PSS_CONTROLLER_BATTERY_PACKET) payload);
        break;
    }
    }
    // Retain dispatcher ownership while yielding to other sessions.
    if (input->input_queue.finishSlice()) task_pool.push(passthrough_next_message, input);
  }

  /**
   * @brief Called on the control stream thread to queue an input message.
   * @param input The input context pointer.
   * @param input_data The input message.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {
    if (input_data.size() < sizeof(NV_INPUT_HEADER)) return;
    auto header = reinterpret_cast<PNV_INPUT_HEADER>(input_data.data());
    size_t required = 0;
    switch (util::endian::little(header->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5: required = sizeof(NV_REL_MOUSE_MOVE_PACKET); break;
      case MOUSE_MOVE_ABS_MAGIC: required = sizeof(NV_ABS_MOUSE_MOVE_PACKET); break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5: required = sizeof(NV_MOUSE_BUTTON_PACKET); break;
      case SCROLL_MAGIC_GEN5: required = sizeof(NV_SCROLL_PACKET); break;
      case SS_HSCROLL_MAGIC: required = sizeof(SS_HSCROLL_PACKET); break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC: required = sizeof(NV_KEYBOARD_PACKET); break;
      case MULTI_CONTROLLER_MAGIC_GEN5: required = sizeof(NV_MULTI_CONTROLLER_PACKET); break;
      case SS_TOUCH_MAGIC: required = sizeof(SS_TOUCH_PACKET); break;
      case SS_PEN_MAGIC: required = sizeof(SS_PEN_PACKET); break;
      case SS_CONTROLLER_ARRIVAL_MAGIC: required = sizeof(SS_CONTROLLER_ARRIVAL_PACKET); break;
      case SS_CONTROLLER_TOUCH_MAGIC: required = sizeof(SS_CONTROLLER_TOUCH_PACKET); break;
      case SS_CONTROLLER_MOTION_MAGIC: required = sizeof(SS_CONTROLLER_MOTION_PACKET); break;
      case SS_CONTROLLER_BATTERY_MAGIC: required = sizeof(SS_CONTROLLER_BATTERY_PACKET); break;
      case UTF8_TEXT_EVENT_MAGIC:
        if (util::endian::big(header->size) < sizeof(header->magic) ||
            util::endian::big(header->size) > input_data.size() - sizeof(header->size)) return;
        required = sizeof(NV_INPUT_HEADER); break;
      default: return;
    }
    if (input_data.size() < required) return;
    if (input->input_queue.enqueue(std::move(input_data))) {
      task_pool.push(passthrough_next_message, input);
    }
  }

  void reset(std::shared_ptr<input_t> &input) {
    std::lock_guard<std::recursive_mutex> dispatch(input->dispatch_lock);
    input->input_queue.reset();
    task_pool.cancel(input->key_press_repeat_id);
    input->key_press_repeat_id = nullptr;
    task_pool.cancel(input->mouse_left_button_timeout);
    input->mouse_left_button_timeout = nullptr;
    platf::android_input::scope scope(input->client_context.get());
    for (auto &gamepad : input->gamepads) {
      task_pool.cancel(gamepad.back_timeout_id);
      task_pool.cancel(gamepad.home_release_id);
      gamepad.home_release_id = nullptr;
      gamepad.back_timeout_id = nullptr;
      if (gamepad.id >= 0) free_gamepad(platf_input, gamepad.id);
      gamepad.id = -1;
      gamepad.gamepad_state = {};
      gamepad.back_button_state = button_state_e::NONE;
    }
    platf::android_input::reset(input->client_context.get());
    input->key_press.clear();
    input->mouse_press.fill(0);
    input->shortcutFlags = 0;
    input->accumulated_vscroll_delta = input->accumulated_hscroll_delta = 0;
  }

  void configure(std::shared_ptr<input_t> &input, int displayId, int width, int height, int rotation) {
    std::lock_guard<std::recursive_mutex> dispatch(input->dispatch_lock);
    reset(input);
    platf::android_input::configure(input->client_context.get(), displayId, width, height, rotation);
  }

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() override {
      platf_input.reset();
    }
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init() {
    platf_input = platf::input();

    return std::make_unique<deinit_t>();
  }

  bool probe_gamepads() {
    const auto gamepads = platf::supported_gamepads(std::addressof(platf_input));
    for (auto &gamepad : gamepads) {
      if (gamepad.is_enabled && gamepad.name != "auto") {
        return false;
      }
    }
    return true;
  }

  std::shared_ptr<input_t> alloc(safe::mail_t mail) {
    auto input = std::make_shared<input_t>(
      mail->event<input::touch_port_t>(mail::touch_port),
      mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback)
    );


    return input;
  }
}  // namespace input
