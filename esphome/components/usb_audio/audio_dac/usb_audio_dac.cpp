#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_audio_dac.h"
#include "esphome/core/log.h"

namespace esphome {
namespace usb_audio {

static const char *const TAG = "usb_audio.audio_dac";

void USBAudioDac::dump_config() {
  ESP_LOGCONFIG(TAG, "USB Audio DAC:");
  ESP_LOGCONFIG(TAG, "  Volume and mute are applied by the device's Feature Unit");
}

bool USBAudioDac::set_mute_off() {
  if (this->parent_ == nullptr)
    return false;
  this->parent_->set_speaker_mute_state(false);
  return true;
}

bool USBAudioDac::set_mute_on() {
  if (this->parent_ == nullptr)
    return false;
  this->parent_->set_speaker_mute_state(true);
  return true;
}

bool USBAudioDac::set_volume(float volume) {
  if (this->parent_ == nullptr)
    return false;
  this->parent_->set_speaker_volume_level(volume);
  return true;
}

// The client holds the requested state, so read it back from there rather than keeping a
// second copy that could drift from it.
bool USBAudioDac::is_muted() { return this->parent_ != nullptr && this->parent_->get_speaker_mute_state(); }

float USBAudioDac::volume() { return this->parent_ != nullptr ? this->parent_->get_speaker_volume_level() : 0.0f; }

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
