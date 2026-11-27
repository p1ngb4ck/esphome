#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"

#include "../ads1115.h"

namespace esphome::ads1115 {

class ADS1115Sensor final : public sensor::Sensor,
                            public PollingComponent,
                            public voltage_sampler::VoltageSampler,
                            public Parented<ADS1115Component> {
 public:
  ADS1115Sensor(uint16_t config_bits, uint8_t conversion_delay_ms)
      : config_bits_(config_bits), conversion_delay_ms_(conversion_delay_ms) {}
#ifdef USE_ESP32
  void setup() override;
  void loop() override;
#endif
  void update() override;
  ADS1115Multiplexer get_multiplexer() const {
    return static_cast<ADS1115Multiplexer>((this->config_bits_ >> 12) & 0b111);
  }
  float sample() override;

  void dump_config() override;

 protected:
#ifdef USE_ESP32
  static void sampling_task_(void *param);
#endif

  const uint16_t config_bits_;
  const uint8_t conversion_delay_ms_;

#ifdef USE_ESP32
  TaskHandle_t sampling_task_handle_{nullptr};
  SemaphoreHandle_t data_mutex_{nullptr};
  volatile bool task_running_{false};
  volatile float cached_voltage_{NAN};
  volatile bool new_data_available_{false};
#endif
};

}  // namespace esphome::ads1115
