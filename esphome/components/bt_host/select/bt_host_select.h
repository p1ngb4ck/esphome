#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

#include <string>
#include <vector>

#include "../bt_host.h"

namespace esphome::bt_host {

/// The devices bt_host.scan finds, of every kind; the one picked is paired. The options change
/// at runtime, so their strings are owned here in a vector reserved once in setup() that the
/// traits point into.
class BTHostSelect : public select::Select, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_parent(BTHost *parent) { this->parent_ = parent; }
  void set_devices(const std::vector<ScannedDevice> &devices);

 protected:
  void control(const std::string &value) override;
  void publish_options_();

  BTHost *parent_{nullptr};
  std::vector<std::string> option_storage_;
};

}  // namespace esphome::bt_host

#endif  // USE_ESP32
