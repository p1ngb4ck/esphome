#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"

#include <vector>
#include <atomic>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace esphome {
namespace ads1115 {

enum ADS1115Multiplexer {
  ADS1115_MULTIPLEXER_P0_N1 = 0b000,
  ADS1115_MULTIPLEXER_P0_N3 = 0b001,
  ADS1115_MULTIPLEXER_P1_N3 = 0b010,
  ADS1115_MULTIPLEXER_P2_N3 = 0b011,
  ADS1115_MULTIPLEXER_P0_NG = 0b100,
  ADS1115_MULTIPLEXER_P1_NG = 0b101,
  ADS1115_MULTIPLEXER_P2_NG = 0b110,
  ADS1115_MULTIPLEXER_P3_NG = 0b111,
};

enum ADS1115Gain {
  ADS1115_GAIN_6P144 = 0b000,
  ADS1115_GAIN_4P096 = 0b001,
  ADS1115_GAIN_2P048 = 0b010,
  ADS1115_GAIN_1P024 = 0b011,
  ADS1115_GAIN_0P512 = 0b100,
  ADS1115_GAIN_0P256 = 0b101,
};

enum ADS1115Resolution {
  ADS1115_16_BITS = 16,
  ADS1015_12_BITS = 12,
};

enum ADS1115Samplerate {
  ADS1115_8SPS = 0b000,
  ADS1115_16SPS = 0b001,
  ADS1115_32SPS = 0b010,
  ADS1115_64SPS = 0b011,
  ADS1115_128SPS = 0b100,
  ADS1115_250SPS = 0b101,
  ADS1115_475SPS = 0b110,
  ADS1115_860SPS = 0b111
};

class ADS1115Component : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  /// HARDWARE_LATE setup priority
  void set_continuous_mode(bool continuous_mode) { continuous_mode_ = continuous_mode; }

  /// Helper method to request a measurement from a sensor.
  float request_measurement(ADS1115Multiplexer multiplexer, ADS1115Gain gain, ADS1115Resolution resolution,
                            ADS1115Samplerate samplerate);

#ifdef USE_ESP32
  /// Lock the ADS1115 for exclusive access (e.g., for multi-sample RMS calculations)
  /// Returns true if lock acquired, false on timeout
  bool lock_adc(uint32_t timeout_ms = 200);

  /// Unlock the ADS1115 after exclusive access
  void unlock_adc();
#endif

 protected:
  uint16_t prev_config_{0};
  bool continuous_mode_;

#ifdef USE_ESP32
  // Channel-aware locking for multi-sample measurements
  std::atomic<bool> measurement_in_progress_{false};
  std::atomic<uint8_t> locked_channel_{0xFF};  // 0xFF = no lock
#endif
};

}  // namespace ads1115
}  // namespace esphome
