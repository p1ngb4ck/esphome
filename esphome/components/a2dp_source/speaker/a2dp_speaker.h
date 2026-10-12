#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../a2dp_source.h"

namespace esphome {
namespace a2dp_source {

// The Bluetooth sink as a speaker: what is played goes out over A2DP, 16 bit stereo at 44.1 kHz.
class A2DPSpeaker : public speaker::Speaker, public Component, public Parented<A2DPSource> {
 public:
  void loop() override;
  void dump_config() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;

  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

 protected:
  void apply_volume_();
};

}  // namespace a2dp_source
}  // namespace esphome

#endif
