#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

#include "esphome/components/usb_audio/usb_audio.h"

#include "esphome/core/component.h"

namespace esphome {
namespace usb_audio {

class USBAudioSpeaker : public USBAudioSink, public Component {
 public:
  explicit USBAudioSpeaker(USBAudioClient *client) : USBAudioSink(client) {}

  void setup() override;
  void dump_config() override;
  void loop() override;
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
