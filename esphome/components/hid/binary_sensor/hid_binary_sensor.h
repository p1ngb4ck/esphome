#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/hid/hid_input.h"

namespace esphome::hid {

class HIDKeyBinarySensor : public binary_sensor::BinarySensor {
 public:
  void attach(HIDInput *parent, uint16_t page, uint16_t usage) {
    this->publish_initial_state(false);
    parent->add_on_key_event_callback([this, page, usage](uint16_t p, uint16_t u, bool pressed) {
      if (p == page && u == usage)
        this->publish_state(pressed);
    });
  }
};

}  // namespace esphome::hid
