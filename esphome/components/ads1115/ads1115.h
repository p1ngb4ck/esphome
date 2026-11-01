#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace esphome::ads1115 {

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

class ADS1115Component final : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  /// HARDWARE_LATE setup priority
  void set_continuous_mode(bool continuous_mode) { continuous_mode_ = continuous_mode; }

  /// Helper method to request a measurement from a sensor.
  float request_measurement(ADS1115Multiplexer multiplexer, ADS1115Gain gain, ADS1115Resolution resolution,
                            ADS1115Samplerate samplerate);

#ifdef USE_ESP32
  /// Perform multiple consecutive measurements on the same channel (for RMS calculations)
  /// Holds a non-blocking lock during the burst to prevent channel switching
  /// @param multiplexer The ADC channel to measure
  /// @param gain The gain setting
  /// @param resolution The resolution (12 or 16 bits)
  /// @param samplerate The sample rate
  /// @param sample_callback Called for each sample with the voltage value
  /// @param num_samples Number of samples to take
  /// @return true if burst completed successfully, false if lock couldn't be acquired
  bool do_burst_measurement(ADS1115Multiplexer multiplexer, ADS1115Gain gain, ADS1115Resolution resolution,
                            ADS1115Samplerate samplerate, std::function<void(float)> sample_callback,
                            uint16_t num_samples);
#endif

 protected:
  float do_measurement_(ADS1115Multiplexer multiplexer, ADS1115Gain gain, ADS1115Resolution resolution,
                        ADS1115Samplerate samplerate);

  uint16_t prev_config_{0};
  bool continuous_mode_;

#ifdef USE_ESP32
  // Non-blocking mutex for burst measurements
  SemaphoreHandle_t burst_mutex_{nullptr};
#endif
};

}  // namespace esphome::ads1115
