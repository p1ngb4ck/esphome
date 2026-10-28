#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/core/component.h"
#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace esphome::bt_offload {

class BTOffload : public Component, public hid::HIDInput {
 public:
  struct Found {
    std::array<uint8_t, 6> address;
    uint8_t transport;
    uint8_t addr_type;
    std::string name;
  };

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void scan(uint8_t seconds);
  bool connect(const std::string &address);
  const std::vector<Found> &found() const { return this->found_; }

  void on_open_(const uint8_t *data, size_t len);
  void on_input_(const uint8_t *data, size_t len);
  void on_close_(const uint8_t *data, size_t len);
  void on_found_(const uint8_t *data, size_t len);

 protected:
  struct Device {
    std::array<uint8_t, 6> address;
    std::string name;
    std::unique_ptr<hid::HIDReportMap> map;
  };
  Device *device_(const uint8_t *address);

  std::vector<Device> devices_;
  std::vector<Found> found_;
};

}

#endif
