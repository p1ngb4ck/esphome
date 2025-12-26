#include "zmpt101b_sensor.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome {
namespace zmpt101b {

static const char *const TAG = "zmpt101b.sensor";

void ZMPT101BSensor::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ZMPT101B...");

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

  // Use configured sensitivity if provided, otherwise use calculated default
  if (this->sensitivity_ > 0.0f) {
    this->calculated_sensitivity_ = this->sensitivity_;
    ESP_LOGI(TAG, "Using configured sensitivity: %.2f mV/V", this->calculated_sensitivity_);
  } else {
    ESP_LOGI(TAG, "Using default sensitivity: %.2f mV/V (calibrate for accuracy)", this->calculated_sensitivity_);
  }
}

void ZMPT101BSensor::update() {
  float rms_voltage = this->calculate_rms_voltage_();

  if (std::isnan(rms_voltage)) {
    ESP_LOGW(TAG, "Failed to read voltage");
    return;
  }

  // Publish voltage to this sensor (inherits from sensor::Sensor)
  this->publish_state(rms_voltage);
}

void ZMPT101BSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "ZMPT101B:");
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "AC Voltage", this);

  ESP_LOGCONFIG(TAG, "  Frequency: %d Hz", this->frequency_);
  ESP_LOGCONFIG(TAG, "  Sensitivity: %.2f mV/V", this->calculated_sensitivity_);
  ESP_LOGCONFIG(TAG, "  Zero Point: %.3f V", this->zero_point_);
  ESP_LOGCONFIG(TAG, "  Samples: %d", this->samples_);
  ESP_LOGCONFIG(TAG, "  Sample Duration: %u ms", this->sample_duration_ms_);
}

float ZMPT101BSensor::get_voltage_sample_() {
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

float ZMPT101BSensor::calculate_rms_voltage_() {
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
      // Get AC component by removing DC offset
      float ac_voltage = voltage - this->zero_point_;

      // Accumulate squared voltage for RMS calculation
      sum_squared += ac_voltage * ac_voltage;
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

  // Calculate RMS of the AC signal: V_rms = sqrt(sum(V^2) / n)
  float v_rms_output = std::sqrt(sum_squared / valid_samples);

  // Convert output voltage RMS to mains voltage RMS using sensitivity
  // Mains voltage = V_output_rms * (1000 / sensitivity_in_mV_per_V)
  float mains_voltage_rms = v_rms_output * (1000.0f / this->calculated_sensitivity_);

  ESP_LOGV(TAG, "RMS Voltage: %.1f V (from %d samples, output RMS: %.3f V)", mains_voltage_rms, valid_samples,
           v_rms_output);

  return mains_voltage_rms;
}

}  // namespace zmpt101b
}  // namespace esphome
