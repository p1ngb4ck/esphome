#include "usb_audio_microphone.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome {
namespace usb_audio {

static const char *const TAG_MIC = "usb_audio.mic";

void USBAudioMicrophone::setup() {
  const EndpointConfig &cfg = this->client_->microphone_config();
  this->audio_stream_info_ = audio::AudioStreamInfo(cfg.bits_per_sample, cfg.channels, cfg.sample_rate);
}

void USBAudioMicrophone::dump_config() {
  const EndpointConfig &cfg = this->client_->microphone_config();
  ESP_LOGCONFIG(TAG_MIC, "USB Microphone:");
  ESP_LOGCONFIG(TAG_MIC, "  Sample rate: %" PRIu32 " Hz", cfg.sample_rate);
  ESP_LOGCONFIG(TAG_MIC, "  Bits per sample: %u", cfg.bits_per_sample);
  ESP_LOGCONFIG(TAG_MIC, "  Source channels: %u", cfg.channels);
  ESP_LOGCONFIG(TAG_MIC, "  Enabled channel mask: 0x%02X", this->enabled_mask_);
  ESP_LOGCONFIG(TAG_MIC, "  Buffer size: %" PRIu32, this->client_->get_microphone_buffer_size());
}

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31
