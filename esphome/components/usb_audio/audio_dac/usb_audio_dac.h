#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/audio_dac/audio_dac.h"
#include "esphome/components/usb_audio/usb_audio.h"
#include "esphome/core/component.h"

namespace esphome {
namespace usb_audio {

// Exposes the Feature Unit of the attached USB audio device as an audio dac, so that
// volume and mute reach the device through the path ESPHome already uses for hardware
// volume: a speaker forwards both to its configured audio_dac.
//
// The setters report whether the request was accepted, not whether the device has applied
// it. A request made while the stream is closed is held and sent once it opens, and a
// device that rejects the control is reported by the client's own logging.
class USBAudioDac : public audio_dac::AudioDac, public Component, public Parented<USBAudioClient> {
 public:
  void dump_config() override;

  bool set_mute_off() override;
  bool set_mute_on() override;
  bool set_volume(float volume) override;

  bool is_muted() override;
  float volume() override;
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
