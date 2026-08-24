#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/usb_audio/usb_audio.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace usb_audio {

class USBAudioSpeaker : public speaker::Speaker, public Component, public Parented<USBAudioClient> {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;

  void start() override;
  void stop() override;
  void finish() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override;
  bool has_buffered_data() const override;

  // Pause keeps the isochronous stream running and feeds it silence, so the device's clock
  // never stops and playback resumes without renegotiating the interface.
  void set_pause_state(bool pause_state) override;
  bool get_pause_state() const override { return this->pause_state_; }

  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }
  void set_bits_per_sample(uint16_t bits) { this->bits_per_sample_ = bits; }
  void set_channels(uint8_t channels) { this->channels_ = channels; }

  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

 protected:
  bool ensure_started_();
  size_t play_internal_(const uint8_t *data, size_t length, uint32_t time_budget_ms);
  // Accumulate the accepted byte count and warn once per window if it falls behind the
  // sample clock. bytes_per_ms is the nominal rate of the configured stream format.
  void check_throughput_(size_t accepted, size_t bytes_per_ms);

  uint32_t sample_rate_{48000};
  uint16_t bits_per_sample_{16};
  uint8_t channels_{2};

  bool pause_state_{false};
  bool finish_requested_{false};
  uint32_t finish_deadline_ms_{0};
  // Throughput window (see check_throughput_). start 0 means "not started yet".
  uint32_t rate_window_start_ms_{0};
  size_t   rate_window_bytes_{0};
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
