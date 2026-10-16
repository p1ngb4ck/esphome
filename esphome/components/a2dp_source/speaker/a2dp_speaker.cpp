#include "a2dp_speaker.h"

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#include "esphome/core/log.h"

#include <esp_timer.h>

#include <algorithm>
#include <cmath>

namespace esphome {
namespace a2dp_source {

static const char *const TAG = "a2dp_source.speaker";

void A2DPSpeaker::loop() {
  if (const uint32_t frames = this->parent_->take_played_frames(); frames != 0)
    this->audio_output_callback_.call(frames, esp_timer_get_time());
  if (this->state_ == speaker::STATE_STOPPING && !this->parent_->has_audio()) {
    this->parent_->set_speaker_active(false);
    this->state_ = speaker::STATE_STOPPED;
  }
}

void A2DPSpeaker::dump_config() { ESP_LOGCONFIG(TAG, "A2DP speaker: 16 bit stereo, 44100 Hz"); }

size_t A2DPSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->state_ != speaker::STATE_RUNNING)
    this->start();
  return this->parent_->write_audio(data, length, ticks_to_wait);
}

void A2DPSpeaker::start() {
  this->parent_->set_speaker_active(true);
  this->state_ = speaker::STATE_RUNNING;
}

void A2DPSpeaker::stop() {
  this->parent_->clear_audio();
  this->parent_->set_speaker_active(false);
  this->state_ = speaker::STATE_STOPPED;
}

void A2DPSpeaker::finish() {
  if (this->state_ == speaker::STATE_RUNNING) {
    this->state_ = speaker::STATE_STOPPING;
    this->parent_->set_speaker_finishing();
  }
}

bool A2DPSpeaker::has_buffered_data() const { return this->parent_->has_audio(); }

void A2DPSpeaker::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);
  this->apply_volume_();
}

void A2DPSpeaker::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);
  this->apply_volume_();
}

void A2DPSpeaker::apply_volume_() {
  const float volume = this->is_silent_() ? 0.0f : std::min(this->volume_, 1.0f);
  this->parent_->set_output_volume(static_cast<uint8_t>(std::lround(volume * 127.0f)));
}

}  // namespace a2dp_source
}  // namespace esphome

#endif
