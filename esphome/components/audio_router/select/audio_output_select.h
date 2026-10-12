#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../speaker/audio_router.h"

namespace esphome::audio_router {

// The router's outputs as a select: picking one switches all sound to it.
class AudioOutputSelect : public select::Select, public Component, public Parented<AudioRouter> {
 public:
  void setup() override;
  void dump_config() override;
  // After the router, which restores the selected output in its setup().
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

 protected:
  void control(size_t index) override;
};

}  // namespace esphome::audio_router

#endif  // USE_ESP32
