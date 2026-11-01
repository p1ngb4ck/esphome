#pragma once

#include "esphome/components/text/text.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_ESP32

namespace esphome::nvs {

class NvsText : public text::Text, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  void set_location(const char *nvs_namespace, const char *key) {
    this->namespace_ = nvs_namespace;
    this->key_ = key;
  }
  void set_initial_value(const char *value) { this->initial_value_ = value; }

 protected:
  void control(const std::string &value) override;

  const char *namespace_{nullptr};
  const char *key_{nullptr};
  const char *initial_value_{""};
};

}  // namespace esphome::nvs

#endif
