#pragma once

#include "esphome/core/automation.h"
#include "hid_input.h"

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

}  // namespace esphome::hid
