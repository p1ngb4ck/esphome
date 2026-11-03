#pragma once
#ifdef USE_ESP32_VARIANT_ESP32P4
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esp_ldo_regulator.h"
#include <utility>

namespace esphome::esp_ldo {

class EspLdo final : public Component {
 public:
  EspLdo(int channel) : channel_(channel) {}

  void setup() override;
  void dump_config() override;

  void set_adjustable(bool adjustable) { this->adjustable_ = adjustable; }
  void set_voltage(int voltage_mv) { this->voltage_mv_ = voltage_mv; }
  void adjust_voltage(float voltage);
  void turn_off();
  void turn_on();
  bool is_on() const { return this->handle_ != nullptr; }
  template<typename F> void add_on_turn_on_callback(F &&cb) { this->on_turn_on_.add(std::forward<F>(cb)); }
  float get_setup_priority() const override {
    return setup_priority::BUS;  // LDO setup should be done early
  }

 protected:
  int channel_;
  int voltage_mv_{2700};
  bool adjustable_{false};
  esp_ldo_channel_handle_t handle_{};
  LazyCallbackManager<void()> on_turn_on_;
};

}  // namespace esphome::esp_ldo

#endif  // USE_ESP32_VARIANT_ESP32P4
