#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"

#include "../ads1115.h"

namespace esphome::ads1115 {

/// Internal holder class that is in instance of Sensor so that the hub can create individual sensors.
class ADS1115Sensor : public sensor::Sensor,
                      public PollingComponent,
                      public voltage_sampler::VoltageSampler,
                      public Parented<ADS1115Component> {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void set_multiplexer(ADS1115Multiplexer multiplexer) { this->multiplexer_ = multiplexer; }
  void set_gain(ADS1115Gain gain) { this->gain_ = gain; }
  void set_resolution(ADS1115Resolution resolution) { this->resolution_ = resolution; }
  void set_samplerate(ADS1115Samplerate samplerate) { this->samplerate_ = samplerate; }
  ADS1115Multiplexer get_multiplexer() const { return this->multiplexer_; }
  float sample() override;

  void dump_config() override;

 protected:
#ifdef USE_ESP32
  static void sampling_task_(void *param);
#endif

  ADS1115Multiplexer multiplexer_;
  ADS1115Gain gain_;
  ADS1115Resolution resolution_;
  ADS1115Samplerate samplerate_;

#ifdef USE_ESP32
  TaskHandle_t sampling_task_handle_{nullptr};
  SemaphoreHandle_t data_mutex_{nullptr};
  volatile bool task_running_{false};
  volatile float cached_voltage_{NAN};
  volatile bool new_data_available_{false};
#endif
};

}  // namespace esphome::ads1115
