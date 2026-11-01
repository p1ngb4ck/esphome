#pragma once

#include "esphome/core/defines.h"

#ifdef USE_HID_API_ACTIONS

#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/component.h"
#include "hid_input.h"

#include <algorithm>
#include <string>

namespace esphome::hid {

// The pad outputs of one HID input as Home Assistant actions: <prefix>_rumble,
// <prefix>_set_player_led, <prefix>_set_light_bar.
class HIDAPIActions : public Component, public api::CustomAPIDevice {
 public:
  HIDAPIActions(HIDInput *input, const char *prefix) : input_(input), prefix_(prefix) {}

  void setup() override {
    this->register_service(&HIDAPIActions::rumble_, this->prefix_ + "_rumble", {"strong", "weak", "duration_ms"});
    this->register_service(&HIDAPIActions::set_player_led_, this->prefix_ + "_set_player_led", {"player"});
    this->register_service(&HIDAPIActions::set_light_bar_, this->prefix_ + "_set_light_bar",
                           {"red", "green", "blue"});
  }
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  // strong, weak: percent; duration 0 holds it.
  void rumble_(int32_t strong, int32_t weak, int32_t duration_ms) {
    auto level = [](int32_t percent) { return static_cast<uint8_t>(std::clamp<int32_t>(percent, 0, 100) * 255 / 100); };
    this->input_->rumble(level(strong), level(weak), static_cast<uint32_t>(std::max<int32_t>(duration_ms, 0)));
  }
  void set_player_led_(int32_t player) {
    this->input_->set_player_led(static_cast<uint8_t>(std::clamp<int32_t>(player, 0, 255)));
  }
  // 0..255 per channel.
  void set_light_bar_(int32_t red, int32_t green, int32_t blue) {
    auto c = [](int32_t v) { return static_cast<uint8_t>(std::clamp<int32_t>(v, 0, 255)); };
    this->input_->set_light_bar(c(red), c(green), c(blue));
  }

  HIDInput *input_;
  std::string prefix_;
};

}  // namespace esphome::hid

#endif  // USE_HID_API_ACTIONS
