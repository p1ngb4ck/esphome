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
namespace zmpt101b {

class ZMPT101BSensor : public sensor::Sensor, public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Configuration setters
  void set_source(voltage_sampler::VoltageSampler *source) { this->voltage_source_ = source; }
  void set_frequency(uint8_t frequency) { this->frequency_ = frequency; }
  void set_sensitivity(float sensitivity) { this->sensitivity_ = sensitivity; }
  void set_zero_point(float zero_point) { this->zero_point_ = zero_point; }
  void set_samples(uint16_t samples) { this->samples_ = samples; }
  void set_sample_duration(uint32_t duration_ms) { this->sample_duration_ms_ = duration_ms; }

 protected:
#ifdef USE_ESP32
  /// @brief FreeRTOS task function for background sampling
  static void sampling_task_(void *param);
#endif
  /// @brief Calculate RMS voltage from multiple samples
  /// @return RMS voltage in volts
  float calculate_rms_voltage_();

  /// @brief Get a single voltage sample from the source
  /// @return Voltage in volts, or NAN if sample failed
  float get_voltage_sample_();

  voltage_sampler::VoltageSampler *voltage_source_{nullptr};

  uint8_t frequency_{50};            // AC mains frequency (50 or 60 Hz)
  float sensitivity_{0.0f};          // mV/V sensitivity (0 = auto-calculate)
  float zero_point_{2.5};            // Voltage at zero AC input
  uint16_t samples_{200};            // Number of samples for RMS calculation
  uint32_t sample_duration_ms_{40};  // Duration to sample over (milliseconds)

  // Runtime calculated values
  float calculated_sensitivity_{8.36};  // Default sensitivity if not specified

#ifdef USE_ESP32
  // FreeRTOS task management
  TaskHandle_t sampling_task_handle_{nullptr};
  SemaphoreHandle_t data_mutex_{nullptr};
  volatile bool task_running_{false};
  volatile float cached_voltage_{NAN};
  volatile bool new_data_available_{false};

  // Global I2C mutex for thread-safe ADC access
  static SemaphoreHandle_t i2c_mutex_;
  static SemaphoreHandle_t get_i2c_mutex_();
#endif
};

}  // namespace zmpt101b
}  // namespace esphome
