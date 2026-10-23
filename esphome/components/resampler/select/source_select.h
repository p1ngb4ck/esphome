#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_RESAMPLER_SOURCE_SELECT)

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../microphone/resampler_microphone.h"

namespace esphome::resampler {

class SourceSelect : public select::Select, public Component, public Parented<ResamplerMicrophone> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  void control(size_t index) override;
  void publish_options_();
  void publish_source_(int index);
};

}  // namespace esphome::resampler

#endif
