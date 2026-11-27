#include "ads1115.h"
#include "esphome/components/ads1xxx/ads1xxx.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome::ads1115 {

ESPHOME_LOG_TAG(TAG, "ads1115");
static constexpr uint8_t REGISTER_CONVERSION = 0x00;
static constexpr uint8_t REGISTER_CONFIG = 0x01;
static constexpr uint16_t CONFIG_COMPARATOR_DISABLE = 0b11;

void ADS1115Component::setup() {
  const uint16_t config = ads1xxx::CONFIG_DATA_RATE | CONFIG_COMPARATOR_DISABLE |
                          (this->continuous_mode_ ? 0 : ads1xxx::CONFIG_SINGLE_SHOT);
  if (!this->setup_check(this->write_byte_16(REGISTER_CONFIG, config)))
    return;
  this->prev_config_ = config;

#ifdef USE_ESP32
  // Create non-blocking mutex for burst measurements
  this->burst_mutex_ = xSemaphoreCreateMutex();
#endif
}

void ADS1115Component::dump_config() {
  ESP_LOGCONFIG(TAG, "ADS1115:");
  LOG_I2C_DEVICE(this);
}

void ADS1115Component::loop() {}

float ADS1115Component::request_measurement(uint16_t config_bits, uint8_t conversion_delay_ms) {
#ifdef USE_ESP32
  // Acquire burst lock (blocking - safe because called from background task)
  // Will wait if RMS measurement is in progress
  if (this->burst_mutex_ == nullptr || xSemaphoreTake(this->burst_mutex_, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire burst lock");
    return NAN;
  }

  // Lock acquired, do measurement
  float result = this->do_measurement_(config_bits, conversion_delay_ms);

  // Release lock
  xSemaphoreGive(this->burst_mutex_);
  return result;
#else
  // Non-ESP32: Direct measurement
  return this->do_measurement_(config_bits, conversion_delay_ms);
#endif
}

#ifdef USE_ESP32
static constexpr uint16_t SPS_16_BITS[8] = {8, 16, 32, 64, 128, 250, 475, 860};
static constexpr uint16_t SPS_12_BITS[8] = {128, 250, 490, 920, 1600, 2400, 3300, 3300};

bool ADS1115Component::do_burst_measurement(ADS1115Multiplexer multiplexer, ADS1115Gain gain,
                                            ADS1115Resolution resolution, ADS1115Samplerate samplerate,
                                            std::function<void(float)> sample_callback, uint16_t num_samples) {
  const uint16_t config_bits = ((multiplexer & 0b111) << 12) | ((gain & 0b111) << 9) | ((samplerate & 0b111) << 5);
  const uint16_t sps = resolution == ADS1015_12_BITS ? SPS_12_BITS[samplerate & 0b111] : SPS_16_BITS[samplerate & 0b111];
  const auto conversion_delay_ms = static_cast<uint8_t>(std::ceil(1000.0f / sps + 0.5f));

  // Acquire lock (blocking - safe because called from background task)
  // Will wait if another measurement is in progress
  if (this->burst_mutex_ == nullptr || xSemaphoreTake(this->burst_mutex_, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire burst lock");
    return false;
  }

  // Lock acquired, perform all samples consecutively
  for (uint16_t i = 0; i < num_samples; i++) {
    float voltage = this->do_measurement_(config_bits, conversion_delay_ms);

    if (std::isnan(voltage)) {
      // Measurement failed, release lock and return
      xSemaphoreGive(this->burst_mutex_);
      ESP_LOGW(TAG, "Burst measurement failed at sample %d/%d", i + 1, num_samples);
      return false;
    }

    // Call callback with sample value
    sample_callback(voltage);

    // Yield every 10 samples to keep system responsive
    if (i % 10 == 0) {
      yield();
    }
  }

  // All samples complete, release lock
  xSemaphoreGive(this->burst_mutex_);
  return true;
}
#endif

float ADS1115Component::do_measurement_(uint16_t config_bits, uint8_t conversion_delay_ms) {
  const uint16_t config = CONFIG_COMPARATOR_DISABLE | config_bits |
                          (this->continuous_mode_ ? 0 : ads1xxx::CONFIG_SINGLE_SHOT | ads1xxx::CONFIG_START);

  if (!this->continuous_mode_ || this->prev_config_ != config) {
    if (!this->status_comm_check(this->write_byte_16(REGISTER_CONFIG, config)))
      return NAN;
    this->prev_config_ = config;
    delay(conversion_delay_ms);

    // In continuous mode the delay alone covers a conversion with the new settings
    if (!this->continuous_mode_) {
      uint32_t start = millis();
      uint16_t status;
      while (true) {
        // A failed read is not a finished conversion: the conversion register still holds the previous
        // result, which can come from another multiplexer input.
        if (!this->status_comm_check(this->read_byte_16(REGISTER_CONFIG, &status)))
          return NAN;
        if (status & ads1xxx::CONFIG_START)
          break;
        if (millis() - start > 100) {
          this->status_comm_result(false, LOG_STR("Conversion timed out"));
          return NAN;
        }
        yield();
      }
    }
  }

  uint16_t raw;
  if (!this->status_comm_result(this->read_byte_16(REGISTER_CONVERSION, &raw)))
    return NAN;
  return ads1xxx::to_volts(static_cast<int16_t>(raw), config_bits);
}

}  // namespace esphome::ads1115
