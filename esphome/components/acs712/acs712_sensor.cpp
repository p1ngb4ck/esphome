#include "acs712_sensor.h"
#include "esphome/components/ads1115/ads1115.h"
#include "esphome/components/ads1115/sensor/ads1115_sensor.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome {
namespace acs712 {

static const char *const TAG = "acs712.sensor";

void ACS712Sensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ACS712...");

  if (this->voltage_source_ == nullptr) {
    ESP_LOGE(TAG, "Voltage source not configured!");
    this->mark_failed();
    return;
  }

  // Perform auto-zero calibration on startup
  ESP_LOGI(TAG, "Performing auto-zero calibration...");
  float zero_sum = 0.0f;
  uint16_t valid_samples = 0;

  // Take 50 samples to determine zero point
  for (uint16_t i = 0; i < 50; i++) {
    float voltage = this->get_voltage_sample_();
    if (!std::isnan(voltage)) {
      zero_sum += voltage;
      valid_samples++;
    }
    delay(10);
  }

  if (valid_samples > 0) {
    this->zero_point_ = zero_sum / valid_samples;
    ESP_LOGI(TAG, "Auto-calibrated zero point: %.3f V (from %d samples)", this->zero_point_, valid_samples);
  } else {
    ESP_LOGW(TAG, "Auto-calibration failed, using configured zero point: %.3f V", this->zero_point_);
  }

#ifdef USE_ESP32
  // Create mutex for thread-safe data access
  this->data_mutex_ = xSemaphoreCreateMutex();
  if (this->data_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create data mutex!");
    this->mark_failed();
    return;
  }

  // Create background sampling task
  this->task_running_ = true;
  BaseType_t result = xTaskCreatePinnedToCore(ACS712Sensor::sampling_task_,  // Task function
                                              "acs712_sample",               // Task name
                                              4096,                          // Stack size (bytes)
                                              this,                          // Task parameter (this pointer)
                                              1,                             // Priority (1 = low priority)
                                              &this->sampling_task_handle_,  // Task handle
                                              0  // Core ID (0 = protocol core for I2C safety)
  );

  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create sampling task!");
    this->task_running_ = false;
    vSemaphoreDelete(this->data_mutex_);
    this->data_mutex_ = nullptr;
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "Background sampling task started on core 1");
#endif
}

void ACS712Sensor::update() {
#ifdef USE_ESP32
  // Trigger background task to perform measurement (non-blocking, returns immediately)
  // Result will be published in loop() when ready
  if (this->sampling_task_handle_ != nullptr) {
    xTaskNotifyGive(this->sampling_task_handle_);
  }
#else
  // On non-ESP32 platforms, use blocking measurement
  float rms_current = this->calculate_rms_current_();

  if (std::isnan(rms_current)) {
    ESP_LOGW(TAG, "Failed to read current");
    return;
  }

  // Publish current to this sensor (inherits from sensor::Sensor)
  this->publish_state(rms_current);

  // Publish power
  if (this->power_sensor_ != nullptr) {
    float power = rms_current * this->line_voltage_;
    this->power_sensor_->publish_state(power);
  }

  // Optionally publish average voltage (for debugging)
  if (this->voltage_sensor_ != nullptr) {
    float avg_voltage = this->get_voltage_sample_();
    if (!std::isnan(avg_voltage)) {
      this->voltage_sensor_->publish_state(avg_voltage);
    }
  }
#endif
}

void ACS712Sensor::dump_config() {
  ESP_LOGCONFIG(TAG, "ACS712:");
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Current", this);

  const char *model_str;
  switch (this->model_) {
    case ACS712_5A:
      model_str = "5A";
      break;
    case ACS712_20A:
      model_str = "20A";
      break;
    case ACS712_30A:
      model_str = "30A";
      break;
    default:
      model_str = "Unknown";
  }

  ESP_LOGCONFIG(TAG, "  Model: ACS712-%s", model_str);
  ESP_LOGCONFIG(TAG, "  Sensitivity: %.3f V/A", this->sensitivity_);
  ESP_LOGCONFIG(TAG, "  Zero Point: %.3f V", this->zero_point_);
  ESP_LOGCONFIG(TAG, "  Line Voltage: %.1f V", this->line_voltage_);
  ESP_LOGCONFIG(TAG, "  Samples: %d", this->samples_);
  ESP_LOGCONFIG(TAG, "  Sample Duration: %u ms", this->sample_duration_ms_);

  LOG_SENSOR("  ", "Power", this->power_sensor_);
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_);
}

void ACS712Sensor::set_model(ACS712Model model) {
  this->model_ = model;

  // Set default sensitivity based on model
  switch (model) {
    case ACS712_5A:
      this->sensitivity_ = 0.185;  // 185 mV/A
      break;
    case ACS712_20A:
      this->sensitivity_ = 0.100;  // 100 mV/A
      break;
    case ACS712_30A:
      this->sensitivity_ = 0.066;  // 66 mV/A
      break;
  }
}

float ACS712Sensor::get_voltage_sample_() {
  if (this->voltage_source_ == nullptr) {
    return NAN;
  }

  // Mutex protection is now handled inside ADS1115::request_measurement()
  float voltage = this->voltage_source_->sample();

  // Validate voltage reading
  if (std::isnan(voltage) || voltage < 0.0f || voltage > 5.5f) {
    return NAN;
  }

  return voltage;
}

float ACS712Sensor::calculate_rms_current_() {
  if (this->voltage_source_ == nullptr) {
    ESP_LOGE(TAG, "No voltage source configured");
    return NAN;
  }

#ifdef USE_ESP32
  // Lock ADS1115 for exclusive access during entire RMS calculation
  // This prevents channel thrashing when multiple sensors sample simultaneously
  auto *ads_sensor = static_cast<ads1115::ADS1115Sensor *>(this->voltage_source_);
  bool locked = false;
  if (ads_sensor != nullptr && ads_sensor->get_parent() != nullptr) {
    locked = ads_sensor->get_parent()->lock_adc(500);  // 500ms timeout for RMS calculation
    if (!locked) {
      ESP_LOGW(TAG, "Failed to acquire ADS1115 lock for RMS calculation");
      return NAN;
    }
  }
#endif

  // Calculate delay between samples to span the desired duration
  uint32_t delay_us = (this->sample_duration_ms_ * 1000) / this->samples_;

  // Ensure minimum delay to prevent overwhelming the ADC
  if (delay_us < 100) {
    delay_us = 100;  // Minimum 100 microseconds between samples
  }

  float sum_squared = 0.0f;
  uint16_t valid_samples = 0;

  // Collect samples over the specified duration
  for (uint16_t i = 0; i < this->samples_; i++) {
    float voltage = this->get_voltage_sample_();

    if (!std::isnan(voltage)) {
      // Convert voltage to instantaneous current
      // I(t) = (V(t) - V_zero) / Sensitivity
      float current = (voltage - this->zero_point_) / this->sensitivity_;

      // Accumulate squared current for RMS calculation
      sum_squared += current * current;
      valid_samples++;
    }

    // Wait before next sample - yield to other tasks for better FreeRTOS cooperation
    // For delays >= 100µs, split into chunks with yield() calls
    if (delay_us >= 100) {
      uint32_t chunks = delay_us / 100;
      uint32_t remainder = delay_us % 100;
      for (uint32_t j = 0; j < chunks; j++) {
        delayMicroseconds(100);
        yield();  // Allow other tasks to run every 100µs
      }
      if (remainder > 0) {
        delayMicroseconds(remainder);
      }
    } else {
      delayMicroseconds(delay_us);
    }
  }

  // Check if we got enough valid samples
  if (valid_samples < (this->samples_ / 2)) {
    ESP_LOGW(TAG, "Too few valid samples: %d/%d", valid_samples, this->samples_);
#ifdef USE_ESP32
    if (locked && ads_sensor != nullptr && ads_sensor->get_parent() != nullptr) {
      ads_sensor->get_parent()->unlock_adc();
    }
#endif
    return NAN;
  }

  // Calculate RMS: I_rms = sqrt(sum(I^2) / n)
  float rms_current = std::sqrt(sum_squared / valid_samples);

  ESP_LOGV(TAG, "RMS Current: %.3f A (from %d samples)", rms_current, valid_samples);

#ifdef USE_ESP32
  // Unlock ADS1115 after measurement complete
  if (locked && ads_sensor != nullptr && ads_sensor->get_parent() != nullptr) {
    ads_sensor->get_parent()->unlock_adc();
  }
#endif

  return rms_current;
}

void ACS712Sensor::loop() {
#ifdef USE_ESP32
  // Check if task is still running
  if (this->sampling_task_handle_ != nullptr && !this->task_running_) {
    ESP_LOGE(TAG, "Background sampling task has stopped unexpectedly!");
    this->mark_failed();
    return;
  }

  // Check for new measurement data and publish immediately when available
  if (this->data_mutex_ != nullptr && xSemaphoreTake(this->data_mutex_, 0) == pdTRUE) {
    if (this->new_data_available_) {
      float rms_current = this->cached_current_;
      this->new_data_available_ = false;
      xSemaphoreGive(this->data_mutex_);

      if (!std::isnan(rms_current)) {
        // Publish current to this sensor (inherits from sensor::Sensor)
        this->publish_state(rms_current);

        // Publish power
        if (this->power_sensor_ != nullptr) {
          float power = rms_current * this->line_voltage_;
          this->power_sensor_->publish_state(power);
        }

        // Optionally publish average voltage (for debugging)
        if (this->voltage_sensor_ != nullptr) {
          float avg_voltage = this->get_voltage_sample_();
          if (!std::isnan(avg_voltage)) {
            this->voltage_sensor_->publish_state(avg_voltage);
          }
        }
      } else {
        ESP_LOGW(TAG, "Failed to read current");
      }
    } else {
      xSemaphoreGive(this->data_mutex_);
    }
  }
#endif
}

#ifdef USE_ESP32
void ACS712Sensor::sampling_task_(void *param) {
  auto *sensor = static_cast<ACS712Sensor *>(param);
  ESP_LOGI(TAG, "Background sampling task started, waiting for triggers");

  while (sensor->task_running_) {
    // Wait indefinitely for notification from update()
    // This blocks without consuming CPU or accessing ADC
    uint32_t notification_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (notification_value > 0 && sensor->task_running_) {
      // Perform ONE measurement cycle when triggered
      float rms_current = sensor->calculate_rms_current_();

      // Store result with mutex protection
      if (sensor->data_mutex_ != nullptr && xSemaphoreTake(sensor->data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        sensor->cached_current_ = rms_current;
        sensor->new_data_available_ = true;
        xSemaphoreGive(sensor->data_mutex_);
      }
    }
  }

  ESP_LOGI(TAG, "Background sampling task stopped");
  vTaskDelete(nullptr);
}
#endif

}  // namespace acs712
}  // namespace esphome
