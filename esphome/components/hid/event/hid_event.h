#pragma once

#include "esphome/components/event/event.h"
#include "esphome/components/hid/hid_input.h"

namespace esphome::hid {

class HIDKeyEvent : public event::Event {
 public:
  void attach(HIDInput *parent) {
    parent->add_on_key_event_callback([this](uint16_t page, uint16_t usage, bool pressed) {
      if (!pressed)
        return;
      const std::string name = key_name(page, usage);
      for (const char *type : this->get_event_types()) {
        if (name == type) {
          this->trigger(type);
          return;
        }
      }
    });
  }
};

}  // namespace esphome::hid
