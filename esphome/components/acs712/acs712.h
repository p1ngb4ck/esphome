#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"

namespace esphome {
namespace acs712 {

enum ACS712Model {
  ACS712_5A,
  ACS712_20A,
  ACS712_30A,
};

class ACS712Component : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Configuration setters
  void set_voltage_source(voltage_sampler::VoltageSampler *source) { this->voltage_source_ = source; }
  void set_model(ACS712Model model);
  void set_sensitivity(float sensitivity) { this->sensitivity_ = sensitivity; }
  void set_zero_point(float zero_point) { this->zero_point_ = zero_point; }
  void set_line_voltage(float line_voltage) { this->line_voltage_ = line_voltage; }
  void set_samples(uint16_t samples) { this->samples_ = samples; }
  void set_sample_duration(uint16_t duration_ms) { this->sample_duration_ms_ = duration_ms; }

  // Sensor setters
  void set_current_sensor(sensor::Sensor *sensor) { this->current_sensor_ = sensor; }
  void set_power_sensor(sensor::Sensor *sensor) { this->power_sensor_ = sensor; }
  void set_voltage_sensor(sensor::Sensor *sensor) { this->voltage_sensor_ = sensor; }

 protected:
  /// @brief Calculate RMS current from multiple voltage samples
  /// @return RMS current in amperes
  float calculate_rms_current_();

  /// @brief Get a single voltage sample from the source
  /// @return Voltage in volts, or NAN if sample failed
  float get_voltage_sample_();

  voltage_sampler::VoltageSampler *voltage_source_{nullptr};

  sensor::Sensor *current_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};

  ACS712Model model_{ACS712_20A};
  float sensitivity_{0.100};         // V/A (default for 20A model)
  float zero_point_{2.5};            // Voltage at zero current
  float line_voltage_{230.0};        // AC line voltage for power calculation
  uint16_t samples_{100};            // Number of samples for RMS calculation
  uint16_t sample_duration_ms_{20};  // Duration to sample over (milliseconds)

  // Auto-calibration
  bool auto_zero_{false};
  uint16_t auto_zero_samples_{0};
  float auto_zero_sum_{0.0f};
};

}  // namespace acs712
}  // namespace esphome
