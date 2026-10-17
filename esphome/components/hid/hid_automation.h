#pragma once

#include "esphome/core/automation.h"
#include "hid_input.h"

#include <algorithm>
#include <string>

namespace esphome::hid {

/// on_key: a key or button went down or up; with a key set, only that one.
class KeyTrigger : public Trigger<std::string, bool> {
 public:
  explicit KeyTrigger(HIDInput *parent) {
    parent->add_on_key_event_callback([this](uint16_t page, uint16_t usage, bool pressed) {
      if (this->any_ || (page == this->page_ && usage == this->usage_))
        this->trigger(key_name(page, usage), pressed);
    });
  }
  void set_key(uint16_t page, uint16_t usage) {
    this->page_ = page;
    this->usage_ = usage;
    this->any_ = false;
  }

 protected:
  uint16_t page_{0};
  uint16_t usage_{0};
  bool any_{true};
};

/// on_text: what a key press typed, after layout, Shift, AltGr, Caps Lock and dead keys.
class TextTrigger : public Trigger<std::string> {
 public:
  explicit TextTrigger(HIDInput *parent) {
    parent->add_on_text_callback([this](const std::string &text) { this->trigger(text); });
  }
};

/// on_pointer: relative motion, wheel, horizontal wheel and the button bits of one report.
class PointerTrigger : public Trigger<int, int, int, int, int> {
 public:
  explicit PointerTrigger(HIDInput *parent) {
    parent->add_on_pointer_callback([this](int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
      this->trigger(dx, dy, wheel, pan, buttons);
    });
  }
};

/// on_control: any other control (stick axes, triggers, hat switch) and its value.
class ControlTrigger : public Trigger<std::string, int> {
 public:
  explicit ControlTrigger(HIDInput *parent) {
    parent->add_on_control_callback(
        [this](uint16_t page, uint16_t usage, int32_t value) { this->trigger(key_name(page, usage), value); });
  }
};

static inline uint8_t output_level(float fraction) {
  return static_cast<uint8_t>(std::clamp(fraction, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/// hid.rumble: both motors, 0..1 each; duration 0 holds it until the next one.
template<typename... Ts> class RumbleAction : public Action<Ts...> {
 public:
  explicit RumbleAction(HIDInput *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(float, strong)
  TEMPLATABLE_VALUE(float, weak)
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override {
    this->parent_->rumble(output_level(this->strong_.value(x...)), output_level(this->weak_.value(x...)),
                          this->duration_.value(x...));
  }

 protected:
  HIDInput *parent_;
};

/// hid.set_player_led: the player number the pad shows, 0 off.
template<typename... Ts> class SetPlayerLedAction : public Action<Ts...> {
 public:
  explicit SetPlayerLedAction(HIDInput *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, player)
  void play(const Ts &...x) override { this->parent_->set_player_led(this->player_.value(x...)); }

 protected:
  HIDInput *parent_;
};

/// hid.set_light_bar: the light bar color, 0..1 per channel.
template<typename... Ts> class SetLightBarAction : public Action<Ts...> {
 public:
  explicit SetLightBarAction(HIDInput *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(float, red)
  TEMPLATABLE_VALUE(float, green)
  TEMPLATABLE_VALUE(float, blue)
  void play(const Ts &...x) override {
    this->parent_->set_light_bar(output_level(this->red_.value(x...)), output_level(this->green_.value(x...)),
                                 output_level(this->blue_.value(x...)));
  }

 protected:
  HIDInput *parent_;
};

}  // namespace esphome::hid
