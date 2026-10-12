#pragma once

#include "esphome/components/hid/hid_input.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <string>

namespace esphome::hid {

enum HIDTextSensorType : uint8_t {
  HID_TEXT_LAST_KEY,
  HID_TEXT_TYPED,
  HID_TEXT_LINE,
};

class HIDTextSensor : public text_sensor::TextSensor {
 public:
  void attach(HIDInput *parent, HIDTextSensorType type) {
    switch (type) {
      case HID_TEXT_LAST_KEY:
        parent->add_on_key_event_callback([this](uint16_t page, uint16_t usage, bool pressed) {
          if (pressed)
            this->publish_state(key_name(page, usage));
        });
        break;
      case HID_TEXT_TYPED:
        parent->add_on_text_callback([this](const std::string &text) { this->publish_state(text); });
        break;
      case HID_TEXT_LINE:
        parent->add_on_text_callback([this](const std::string &text) {
          if (text == "\n") {
            this->publish_state(this->line_);
            this->line_.clear();
          } else {
            this->line_ += text;
          }
        });
        // Backspace takes back the last character typed (UTF-8 aware).
        parent->add_on_key_event_callback([this](uint16_t page, uint16_t usage, bool pressed) {
          if (!pressed || page != PAGE_KEYBOARD || usage != 0x2A)
            return;
          while (!this->line_.empty()) {
            const char c = this->line_.back();
            this->line_.pop_back();
            if ((c & 0xC0) != 0x80)
              break;
          }
        });
        break;
    }
  }

 protected:
  std::string line_;
};

}  // namespace esphome::hid
