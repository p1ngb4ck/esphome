#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../speaker/resampler_speaker.h"

namespace esphome::resampler {

class OutputSelect : public select::Select, public Component, public Parented<ResamplerSpeaker> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  void control(size_t index) override;
};

}

#endif
