#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

#include <string>
#include <vector>

#include "../ble_client_hid.h"

namespace esphome::ble_client_hid {

/// The devices a pairing scan finds; the one picked is paired. The options change at runtime, so
/// their strings are owned here in a vector reserved once in setup() that the traits point into.
class BLEHIDDeviceSelect : public select::Select, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_parent(BLEClientHID *parent) { this->parent_ = parent; }
  void set_devices(const std::vector<FoundBLEDevice> &devices);

 protected:
  void control(const std::string &value) override;
  void publish_options_();

  BLEClientHID *parent_{nullptr};
  std::vector<std::string> option_storage_;
};

}  // namespace esphome::ble_client_hid

#endif  // USE_ESP32
