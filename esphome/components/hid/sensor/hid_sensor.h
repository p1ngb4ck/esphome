#pragma once

#include "esphome/components/hid/hid_input.h"
#include "esphome/components/sensor/sensor.h"

#include <algorithm>
#include <cstdint>

namespace esphome::hid {

class HIDControlSensor : public sensor::Sensor {
 public:
  void attach(HIDInput *parent, uint16_t page, uint16_t usage) {
    // Sticks and steering are -32768..32767, triggers, sliders and pedals 0..65535 (see
    // HIDInput::gamepad_axis_()); everything else is passed on as it is.
    const bool stick = (page == 0x01 && (usage == 0x30 || usage == 0x31 || usage == 0x33 || usage == 0x34)) ||
                       (page == 0x02 && usage == 0xC8);
    const bool trigger = (page == 0x01 && (usage == 0x32 || usage == 0x35 || usage == 0x36 || usage == 0x37)) ||
                         (page == 0x02 && !stick);
    parent->add_on_control_callback([this, page, usage, stick, trigger](uint16_t p, uint16_t u, int32_t value) {
      if (p != page || u != usage)
        return;
      int32_t shown = value;
      if (stick) {
        shown = std::clamp<int32_t>((value * 100 + (value >= 0 ? 16384 : -16384)) / 32768, -100, 100);
      } else if (trigger) {
        shown = std::clamp<int32_t>((value * 100 + 32767) / 65535, 0, 100);
      }
      // Only what changes what is shown: a stick reports hundreds of times a second.
      if (this->has_state() && shown == this->last_)
        return;
      this->last_ = shown;
      this->publish_state(static_cast<float>(shown));
    });
  }

 protected:
  int32_t last_{0};
};

}  // namespace esphome::hid
