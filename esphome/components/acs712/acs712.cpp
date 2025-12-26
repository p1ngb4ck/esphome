#include "acs712.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome {
namespace acs712 {

static const char *const TAG = "acs712";

void ACS712Component::setup() {
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
}

void ACS712Component::update() {
  float rms_current = this->calculate_rms_current_();

  if (std::isnan(rms_current)) {
    ESP_LOGW(TAG, "Failed to read current");
    return;
  }

  // Publish current
  if (this->current_sensor_ != nullptr) {
    this->current_sensor_->publish_state(rms_current);
  }

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
}

void ACS712Component::dump_config() {
  ESP_LOGCONFIG(TAG, "ACS712:");
  LOG_UPDATE_INTERVAL(this);

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
  ESP_LOGCONFIG(TAG, "  Sample Duration: %d ms", this->sample_duration_ms_);

  LOG_SENSOR("  ", "Current", this->current_sensor_);
  LOG_SENSOR("  ", "Power", this->power_sensor_);
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_);
}

void ACS712Component::set_model(ACS712Model model) {
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

float ACS712Component::get_voltage_sample_() {
  if (this->voltage_source_ == nullptr) {
    return NAN;
  }

  float voltage = this->voltage_source_->sample();

  // Validate voltage reading
  if (std::isnan(voltage) || voltage < 0.0f || voltage > 5.5f) {
    return NAN;
  }

  return voltage;
}

float ACS712Component::calculate_rms_current_() {
  if (this->voltage_source_ == nullptr) {
    ESP_LOGE(TAG, "No voltage source configured");
    return NAN;
  }

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

    // Wait before next sample
    delayMicroseconds(delay_us);
  }

  // Check if we got enough valid samples
  if (valid_samples < (this->samples_ / 2)) {
    ESP_LOGW(TAG, "Too few valid samples: %d/%d", valid_samples, this->samples_);
    return NAN;
  }

  // Calculate RMS: I_rms = sqrt(sum(I^2) / n)
  float rms_current = std::sqrt(sum_squared / valid_samples);

  ESP_LOGV(TAG, "RMS Current: %.3f A (from %d samples)", rms_current, valid_samples);

  return rms_current;
}

}  // namespace acs712
}  // namespace esphome
