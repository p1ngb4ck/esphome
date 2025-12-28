#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif

namespace esphome {
namespace acs712 {

enum ACS712Model {
  ACS712_5A,
  ACS712_20A,
  ACS712_30A,
};

class ACS712Sensor : public sensor::Sensor, public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Configuration setters
  void set_source(voltage_sampler::VoltageSampler *source) { this->voltage_source_ = source; }
  void set_model(ACS712Model model);
  void set_sensitivity(float sensitivity) { this->sensitivity_ = sensitivity; }
  void set_zero_point(float zero_point) { this->zero_point_ = zero_point; }
  void set_line_voltage(float line_voltage) { this->line_voltage_ = line_voltage; }
  void set_line_voltage_sensor(sensor::Sensor *sensor) { this->line_voltage_sensor_ = sensor; }
  void set_samples(uint16_t samples) { this->samples_ = samples; }
  void set_sample_duration(uint32_t duration_ms) { this->sample_duration_ms_ = duration_ms; }

  // Optional sensor setters
  void set_power_sensor(sensor::Sensor *sensor) { this->power_sensor_ = sensor; }
  void set_voltage_sensor(sensor::Sensor *sensor) { this->voltage_sensor_ = sensor; }

 protected:
#ifdef USE_ESP32
  /// @brief FreeRTOS task function for background sampling
  static void sampling_task_(void *param);
#endif
  /// @brief Calculate RMS current from multiple voltage samples
  /// @return RMS current in amperes
  float calculate_rms_current_();

  /// @brief Get a single voltage sample from the source
  /// @return Voltage in volts, or NAN if sample failed
  float get_voltage_sample_();

  /// @brief Get the current line voltage (from static value or sensor)
  /// @return Line voltage in volts
  float get_line_voltage_();

  voltage_sampler::VoltageSampler *voltage_source_{nullptr};

  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *line_voltage_sensor_{nullptr};

  ACS712Model model_{ACS712_20A};
  float sensitivity_{0.100};         // V/A (default for 20A model)
  float zero_point_{2.5};            // Voltage at zero current
  float line_voltage_{0.0};          // AC line voltage for power calculation (static value)
  uint16_t samples_{100};            // Number of samples for RMS calculation
  uint32_t sample_duration_ms_{20};  // Duration to sample over (milliseconds)

  // Auto-calibration
  bool calibration_complete_{false};
  bool auto_zero_{false};
  uint16_t auto_zero_samples_{0};
  float auto_zero_sum_{0.0f};

#ifdef USE_ESP32
  // FreeRTOS task management
  TaskHandle_t sampling_task_handle_{nullptr};
  SemaphoreHandle_t data_mutex_{nullptr};
  volatile bool task_running_{false};
  volatile float cached_current_{NAN};
  volatile bool new_data_available_{false};
#endif
};

}  // namespace acs712
}  // namespace esphome
