#include "gamepad.h"
#include "hid_input.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::hid {

static const char *const TAG = "hid.gamepad";

static constexpr uint16_t PAGE_SIMULATION = 0x02;
static constexpr uint16_t AXIS_USAGES[PAD_AXIS_COUNT] = {0x30, 0x31, 0x33, 0x34, 0x32, 0x35};
static constexpr uint16_t USAGE_DPAD_UP = 0x90;
// The touchpad's fingers, as controls on the Digitizer page (keys.py names them): the contact count,
// each finger down (tip switch, secondary tip switch) and its x and y (0xF0.., reserved there).
static constexpr uint16_t USAGE_CONTACT_COUNT = 0x54;
static constexpr uint16_t USAGE_FINGER_DOWN[PAD_TOUCH_COUNT] = {0x42, 0x43};
static constexpr uint16_t USAGE_FINGER_X[PAD_TOUCH_COUNT] = {0xF0, 0xF2};
static constexpr uint16_t USAGE_FINGER_Y[PAD_TOUCH_COUNT] = {0xF1, 0xF3};
static const char *const RUMBLE_TIMEOUT = "hid_rumble";

int32_t pad_stick(int32_t v, int32_t min, int32_t max) {
  if (max <= min)
    return 0;
  const int64_t span = static_cast<int64_t>(max) - min + 1;
  const int64_t twice_center = static_cast<int64_t>(min) + max + 1;
  const int64_t r = (2 * static_cast<int64_t>(v) - twice_center) * 65536 / (2 * span);
  return static_cast<int32_t>(std::clamp<int64_t>(r, -32768, 32767));
}

int32_t pad_trigger(int32_t v, int32_t min, int32_t max) {
  if (max <= min)
    return 0;
  const int64_t r = (static_cast<int64_t>(v) - min) * 65535 / (static_cast<int64_t>(max) - min);
  return static_cast<int32_t>(std::clamp<int64_t>(r, 0, 65535));
}

uint8_t pad_hat(int32_t direction) {
  static constexpr uint8_t HAT[8] = {
      DPAD_UP,   DPAD_UP | DPAD_RIGHT,  DPAD_RIGHT, DPAD_DOWN | DPAD_RIGHT,
      DPAD_DOWN, DPAD_DOWN | DPAD_LEFT, DPAD_LEFT,  DPAD_UP | DPAD_LEFT,
  };
  return direction >= 0 && direction < 8 ? HAT[direction] : 0;
}

void HIDInput::gamepad_state(const GamepadState &state) {
  const uint32_t buttons = state.buttons ^ this->pad_state_.buttons;
  for (uint8_t i = 0; i < PAD_BUTTON_COUNT; i++) {
    if ((buttons >> i) & 1)
      this->key_(PAGE_BUTTON, i + 1, (state.buttons >> i) & 1);
  }
  const uint8_t dpad = state.dpad ^ this->pad_state_.dpad;
  for (uint8_t i = 0; i < 4; i++) {
    if ((dpad >> i) & 1)
      this->key_(PAGE_GENERIC_DESKTOP, USAGE_DPAD_UP + i, (state.dpad >> i) & 1);
  }
  for (uint8_t i = 0; i < PAD_AXIS_COUNT; i++) {
    if (state.axes[i] != this->pad_state_.axes[i])
      this->control_cb_.call(PAGE_GENERIC_DESKTOP, AXIS_USAGES[i], state.axes[i]);
  }
  uint8_t contacts = 0;
  uint8_t contacts_before = 0;
  for (uint8_t i = 0; i < PAD_TOUCH_COUNT; i++) {
    const GamepadTouch &now = state.touch[i];
    const GamepadTouch &before = this->pad_state_.touch[i];
    contacts += now.down;
    contacts_before += before.down;
    if (now.down != before.down)
      this->control_cb_.call(PAGE_DIGITIZER, USAGE_FINGER_DOWN[i], now.down);
    if (now.down && (now.x != before.x || !before.down))
      this->control_cb_.call(PAGE_DIGITIZER, USAGE_FINGER_X[i], now.x);
    if (now.down && (now.y != before.y || !before.down))
      this->control_cb_.call(PAGE_DIGITIZER, USAGE_FINGER_Y[i], now.y);
  }
  if (contacts != contacts_before)
    this->control_cb_.call(PAGE_DIGITIZER, USAGE_CONTACT_COUNT, contacts);
  this->pad_state_ = state;
}

void HIDInput::gamepad_hat_(const HIDReportItemValue &v) {
  const int32_t range = v.logical_max - v.logical_min + 1;
  int32_t direction = static_cast<int32_t>(v.value) - v.logical_min;
  if (direction < 0 || direction >= range) {
    direction = -1;
  } else if (range != 8) {
    direction = direction * 8 / range;
  }
  const uint8_t bits = pad_hat(direction);
  for (uint8_t i = 0; i < 4; i++) {
    const bool on = (bits >> i) & 1;
    if (on != this->is_key_down(PAGE_GENERIC_DESKTOP, USAGE_DPAD_UP + i))
      this->key_(PAGE_GENERIC_DESKTOP, USAGE_DPAD_UP + i, on);
  }
}

// Absolute axes of joysticks and gamepads, by their logical range. A gamepad's descriptor is
// read the DirectInput way (right stick on Z/Rz, triggers on Rx/Ry or Brake/Accelerator) and
// reported in the canonical layout; joysticks and wheels keep their usages.
bool HIDInput::gamepad_axis_(const HIDReportItemValue &v) {
  if (v.application_usage.page != PAGE_GENERIC_DESKTOP)
    return false;
  const bool gamepad = v.application_usage.usage == 0x05;
  if (!gamepad && v.application_usage.usage != 0x04 && v.application_usage.usage != 0x08)
    return false;
  uint16_t page = v.usage.page;
  uint16_t usage = v.usage.usage;
  bool stick;
  if (page == PAGE_GENERIC_DESKTOP) {
    switch (usage) {
      case 0x30:
      case 0x31:
        stick = true;
        break;
      case 0x32:  // Z
        stick = gamepad;
        usage = gamepad ? 0x33 : usage;
        break;
      case 0x35:  // Rz
        stick = gamepad;
        usage = gamepad ? 0x34 : usage;
        break;
      case 0x33:  // Rx
        stick = !gamepad;
        usage = gamepad ? 0x32 : usage;
        break;
      case 0x34:  // Ry
        stick = !gamepad;
        usage = gamepad ? 0x35 : usage;
        break;
      case 0x36:  // Slider
      case 0x37:  // Dial
        stick = false;
        break;
      default:
        return false;
    }
  } else if (page == PAGE_SIMULATION) {
    switch (usage) {
      case 0xC4:  // Accelerator
      case 0xC5:  // Brake
        stick = false;
        if (gamepad) {
          page = PAGE_GENERIC_DESKTOP;
          usage = v.usage.usage == 0xC5 ? 0x32 : 0x35;
        }
        break;
      case 0xBA:  // Rudder
      case 0xBB:  // Throttle
      case 0xC6:  // Clutch
        stick = false;
        break;
      case 0xC8:  // Steering
        stick = true;
        break;
      default:
        return false;
    }
  } else {
    return false;
  }
  const auto raw = static_cast<int32_t>(v.value);
  const int32_t value = stick ? pad_stick(raw, v.logical_min, v.logical_max) : pad_trigger(raw, v.logical_min, v.logical_max);
  if (gamepad && page == PAGE_GENERIC_DESKTOP) {
    for (uint8_t i = 0; i < PAD_AXIS_COUNT; i++) {
      if (AXIS_USAGES[i] == usage)
        this->pad_state_.axes[i] = value;
    }
  }
  this->control_cb_.call(page, usage, value);
  return true;
}

void HIDInput::rumble(uint8_t strong, uint8_t weak, uint32_t duration_ms) {
  this->pad_outputs_.strong = strong;
  this->pad_outputs_.weak = weak;
  if (this->pad_ != nullptr)
    this->pad_->outputs(this->pad_outputs_);
  Component *component = this->hid_component();
  if (component == nullptr)
    return;
  if (duration_ms != 0 && (strong != 0 || weak != 0)) {
    App.scheduler.set_timeout(component, RUMBLE_TIMEOUT, duration_ms, [this]() { this->rumble(0, 0, 0); });
  } else {
    App.scheduler.cancel_timeout(component, RUMBLE_TIMEOUT);
  }
}

void HIDInput::set_player_led(uint8_t player) {
  this->pad_outputs_.player = player;
  if (this->pad_ != nullptr)
    this->pad_->outputs(this->pad_outputs_);
}

void HIDInput::set_light_bar(uint8_t red, uint8_t green, uint8_t blue) {
  this->pad_outputs_.red = red;
  this->pad_outputs_.green = green;
  this->pad_outputs_.blue = blue;
  if (this->pad_ != nullptr)
    this->pad_->outputs(this->pad_outputs_);
}

void HIDInput::gamepad_attach_(HIDBus bus, uint16_t vid, uint16_t pid, uint16_t version) {
  this->gamepad_attach_(bus, make_gamepad_driver(this, bus, vid, pid, version));
}

void HIDInput::gamepad_attach_(HIDBus bus, std::unique_ptr<GamepadDriver> driver) {
  this->gamepad_detach_();
  this->bus_ = bus;
  this->pad_ = std::move(driver);
  if (this->pad_ != nullptr)
    ESP_LOGI(TAG, "[%s] %s", this->log_name_, this->pad_->name());
}

void HIDInput::gamepad_start_() {
  if (this->pad_ != nullptr)
    this->pad_->start();
}

void HIDInput::gamepad_detach_() {
  this->gamepad_state(GamepadState{});
  this->pad_.reset();
  if (Component *component = this->hid_component())
    App.scheduler.cancel_timeout(component, RUMBLE_TIMEOUT);
}

bool HIDInput::gamepad_report_(const uint8_t *data, size_t len) {
  return this->pad_ != nullptr && len != 0 && this->pad_->report(data, len);
}

void HIDInput::gamepad_feature_(uint8_t id, const uint8_t *data, size_t len) {
  if (this->pad_ != nullptr)
    this->pad_->feature(id, data, len);
}

}  // namespace esphome::hid
