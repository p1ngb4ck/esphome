#include "usb_audio_speaker.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome {
namespace usb_audio {

static const char *const TAG_SPK = "usb_audio.spk";

namespace {
// No audio handed over for this long: the speaker stops itself, as the I2S speaker does after its
// timeout (a mixer never stops its output), so the stream's idle close can follow.
constexpr uint32_t SELF_STOP_MS = 1000;
}  // namespace

void USBAudioSpeaker::setup() {
  const EndpointConfig &cfg = this->client_->speaker_config();
  this->audio_stream_info_ = audio::AudioStreamInfo(cfg.bits_per_sample, cfg.channels, cfg.sample_rate);
}

void USBAudioSpeaker::dump_config() {
  const EndpointConfig &cfg = this->client_->speaker_config();
  ESP_LOGCONFIG(TAG_SPK, "USB Speaker:");
  ESP_LOGCONFIG(TAG_SPK, "  Sample rate: %" PRIu32 " Hz", cfg.sample_rate);
  ESP_LOGCONFIG(TAG_SPK, "  Bits per sample: %u", cfg.bits_per_sample);
  ESP_LOGCONFIG(TAG_SPK, "  Channels: %u", cfg.channels);
}

void USBAudioSpeaker::loop() {
  if (this->state_ == speaker::STATE_RUNNING && !this->get_pause_state() && !this->finishing() &&
      millis() - this->last_play_ms() > SELF_STOP_MS) {
    this->stop();
  }
}

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31
