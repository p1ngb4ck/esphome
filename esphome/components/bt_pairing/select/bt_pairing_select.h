#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

#include <string>
#include <vector>

#include "../bt_pairing.h"

namespace esphome::bt_pairing {

/// The devices bt_pairing.scan finds, of every kind; the one picked is paired. The options change
/// at runtime, so their strings are owned here in a vector reserved once in setup() that the
/// traits point into.
class BTPairingSelect : public select::Select, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_parent(BTPairing *parent) { this->parent_ = parent; }
  void set_devices(const std::vector<ScannedDevice> &devices);

 protected:
  void control(const std::string &value) override;
  void publish_options_();

  BTPairing *parent_{nullptr};
  std::vector<std::string> option_storage_;
};

}  // namespace esphome::bt_pairing

#endif  // USE_ESP32
