#include "ads1115.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ads1115 {

static const char *const TAG = "ads1115";
static const uint8_t ADS1115_REGISTER_CONVERSION = 0x00;
static const uint8_t ADS1115_REGISTER_CONFIG = 0x01;

#ifdef USE_ESP32
// Global mutex for thread-safe ADS1115 sampling across all channels
// This protects the multi-step request_measurement() sequence from concurrent access
SemaphoreHandle_t global_ads1115_mutex = nullptr;

SemaphoreHandle_t get_ads1115_mutex() {
  if (global_ads1115_mutex == nullptr) {
    global_ads1115_mutex = xSemaphoreCreateRecursiveMutex();
  }
  return global_ads1115_mutex;
}
#endif

void ADS1115Component::setup() {
  uint16_t value;
  if (!this->read_byte_16(ADS1115_REGISTER_CONVERSION, &value)) {
    this->mark_failed();
    return;
  }

  uint16_t config = 0;
  // Clear single-shot bit
  //        0b0xxxxxxxxxxxxxxx
  config |= 0b0000000000000000;
  // Setup multiplexer
  //        0bx000xxxxxxxxxxxx
  config |= ADS1115_MULTIPLEXER_P0_N1 << 12;

  // Setup Gain
  //        0bxxxx000xxxxxxxxx
  config |= ADS1115_GAIN_6P144 << 9;

  if (this->continuous_mode_) {
    // Set continuous mode
    //        0bxxxxxxx0xxxxxxxx
    config |= 0b0000000000000000;
  } else {
    // Set singleshot mode
    //        0bxxxxxxx1xxxxxxxx
    config |= 0b0000000100000000;
  }

  // Set data rate - 860 samples per second
  //        0bxxxxxxxx100xxxxx
  config |= ADS1115_860SPS << 5;

  // Set comparator mode - hysteresis
  //        0bxxxxxxxxxxx0xxxx
  config |= 0b0000000000000000;

  // Set comparator polarity - active low
  //        0bxxxxxxxxxxxx0xxx
  config |= 0b0000000000000000;

  // Set comparator latch enabled - false
  //        0bxxxxxxxxxxxxx0xx
  config |= 0b0000000000000000;

  // Set comparator que mode - disabled
  //        0bxxxxxxxxxxxxxx11
  config |= 0b0000000000000011;

  if (!this->write_byte_16(ADS1115_REGISTER_CONFIG, config)) {
    this->mark_failed();
    return;
  }
  this->prev_config_ = config;
}
void ADS1115Component::dump_config() {
  ESP_LOGCONFIG(TAG, "ADS1115:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, ESP_LOG_MSG_COMM_FAIL);
  }
}
float ADS1115Component::request_measurement(ADS1115Multiplexer multiplexer, ADS1115Gain gain,
                                            ADS1115Resolution resolution, ADS1115Samplerate samplerate) {
#ifdef USE_ESP32
  // Acquire recursive mutex for thread-safe access
  // This allows RMS calculations to lock_adc() once and make multiple request_measurement() calls
  SemaphoreHandle_t mutex = get_ads1115_mutex();
  bool acquired = false;
  if (mutex != nullptr) {
    acquired = (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(200)) == pdTRUE);
    if (!acquired) {
      ESP_LOGW(TAG, "Failed to acquire ADS1115 mutex in request_measurement");
      return NAN;
    }
  }
#endif

  uint16_t config = this->prev_config_;
  // Multiplexer
  //        0bxBBBxxxxxxxxxxxx
  config &= 0b1000111111111111;
  config |= (multiplexer & 0b111) << 12;

  // Gain
  //        0bxxxxBBBxxxxxxxxx
  config &= 0b1111000111111111;
  config |= (gain & 0b111) << 9;

  // Sample rate
  //        0bxxxxxxxxBBBxxxxx
  config &= 0b1111111100011111;
  config |= (samplerate & 0b111) << 5;

  if (!this->continuous_mode_) {
    // Start conversion
    config |= 0b1000000000000000;
  }

  if (!this->continuous_mode_ || this->prev_config_ != config) {
    if (!this->write_byte_16(ADS1115_REGISTER_CONFIG, config)) {
      this->status_set_warning();
#ifdef USE_ESP32
      if (acquired && mutex != nullptr) {
        xSemaphoreGiveRecursive(mutex);
      }
#endif
      return NAN;
    }
    this->prev_config_ = config;

    // Delay calculated as: ceil((1000/SPS)+.5)
    if (resolution == ADS1015_12_BITS) {
      switch (samplerate) {
        case ADS1115_8SPS:
          delay(9);
          break;
        case ADS1115_16SPS:
          delay(5);
          break;
        case ADS1115_32SPS:
          delay(3);
          break;
        case ADS1115_64SPS:
        case ADS1115_128SPS:
          delay(2);
          break;
        default:
          delay(1);
          break;
      }
    } else {
      switch (samplerate) {
        case ADS1115_8SPS:
          delay(126);  // NOLINT
          break;
        case ADS1115_16SPS:
          delay(63);  // NOLINT
          break;
        case ADS1115_32SPS:
          delay(32);
          break;
        case ADS1115_64SPS:
          delay(17);
          break;
        case ADS1115_128SPS:
          delay(9);
          break;
        case ADS1115_250SPS:
          delay(5);
          break;
        case ADS1115_475SPS:
          delay(3);
          break;
        case ADS1115_860SPS:
          delay(2);
          break;
      }
    }

    // in continuous mode, conversion will always be running, rely on the delay
    // to ensure conversion is taking place with the correct settings
    // can we use the rdy pin to trigger when a conversion is done?
    if (!this->continuous_mode_) {
      uint32_t start = millis();
      while (this->read_byte_16(ADS1115_REGISTER_CONFIG, &config) && (config >> 15) == 0) {
        if (millis() - start > 100) {
          ESP_LOGW(TAG, "Reading ADS1115 timed out");
          this->status_set_warning();
#ifdef USE_ESP32
          if (acquired && mutex != nullptr) {
            xSemaphoreGiveRecursive(mutex);
          }
#endif
          return NAN;
        }
        yield();
      }
    }
  }

  uint16_t raw_conversion;
  if (!this->read_byte_16(ADS1115_REGISTER_CONVERSION, &raw_conversion)) {
    this->status_set_warning();
#ifdef USE_ESP32
    if (acquired && mutex != nullptr) {
      xSemaphoreGiveRecursive(mutex);
    }
#endif
    return NAN;
  }

  if (resolution == ADS1015_12_BITS) {
    bool negative = (raw_conversion >> 15) == 1;

    // shift raw_conversion as it's only 12-bits, left justified
    raw_conversion = raw_conversion >> (16 - ADS1015_12_BITS);

    // check if number was negative in order to keep the sign
    if (negative) {
      // the number was negative
      // 1) set the negative bit back
      raw_conversion |= 0x8000;
      // 2) reset the former (shifted) negative bit
      raw_conversion &= 0xF7FF;
    }
  }

  auto signed_conversion = static_cast<int16_t>(raw_conversion);

  float millivolts;
  float divider = (resolution == ADS1115_16_BITS) ? 32768.0f : 2048.0f;
  switch (gain) {
    case ADS1115_GAIN_6P144:
      millivolts = (signed_conversion * 6144) / divider;
      break;
    case ADS1115_GAIN_4P096:
      millivolts = (signed_conversion * 4096) / divider;
      break;
    case ADS1115_GAIN_2P048:
      millivolts = (signed_conversion * 2048) / divider;
      break;
    case ADS1115_GAIN_1P024:
      millivolts = (signed_conversion * 1024) / divider;
      break;
    case ADS1115_GAIN_0P512:
      millivolts = (signed_conversion * 512) / divider;
      break;
    case ADS1115_GAIN_0P256:
      millivolts = (signed_conversion * 256) / divider;
      break;
    default:
      millivolts = NAN;
  }

  this->status_clear_warning();

#ifdef USE_ESP32
  // Release recursive mutex
  if (acquired && mutex != nullptr) {
    xSemaphoreGiveRecursive(mutex);
  }
#endif

  return millivolts / 1e3f;
}

#ifdef USE_ESP32
bool ADS1115Component::lock_adc(uint32_t timeout_ms) {
  SemaphoreHandle_t mutex = get_ads1115_mutex();
  if (mutex != nullptr) {
    return xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
  }
  return true;  // No mutex means no ESP32, allow access
}

void ADS1115Component::unlock_adc() {
  SemaphoreHandle_t mutex = get_ads1115_mutex();
  if (mutex != nullptr) {
    xSemaphoreGiveRecursive(mutex);
  }
}
#endif

}  // namespace ads1115
}  // namespace esphome
