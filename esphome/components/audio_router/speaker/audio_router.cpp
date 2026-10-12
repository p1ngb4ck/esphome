#include "audio_router.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::audio_router {

static const char *const TAG = "audio_router";

void AudioRouter::add_output(speaker::Speaker *output, const char *name) {
  const size_t index = this->outputs_.size();
  this->outputs_.push_back({output, name});
  output->add_audio_output_callback([this, index](uint32_t frames, int64_t timestamp) {
    if (this->active_.load() == index)
      this->audio_output_callback_.call(frames, timestamp);
  });
}

void AudioRouter::setup() {
  this->pref_ = global_preferences->make_preference<uint32_t>(this->preference_key_);
  uint32_t stored = 0;
  if (this->pref_.load(&stored) && stored < this->outputs_.size())
    this->active_.store(stored);
  this->pending_ = this->active_.load();
  ESP_LOGD(TAG, "Output: %s", this->output_name(this->active_.load()));
}

void AudioRouter::loop() {
  if (this->switching_.load() && this->in_play_.load() == 0)
    this->switch_now_();
  if (this->state_ == speaker::STATE_STOPPING && this->current_()->is_stopped())
    this->state_ = speaker::STATE_STOPPED;
}

void AudioRouter::dump_config() {
  ESP_LOGCONFIG(TAG, "Audio router:");
  for (size_t i = 0; i < this->outputs_.size(); i++)
    ESP_LOGCONFIG(TAG, "  Output %u: %s%s", static_cast<unsigned>(i), this->outputs_[i].name,
                  i == this->active_.load() ? " (selected)" : "");
}

size_t AudioRouter::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  this->in_play_.fetch_add(1);
  size_t written = 0;
  if (!this->switching_.load()) {
    if (this->state_ != speaker::STATE_RUNNING)
      this->start();
    written = this->current_()->play(data, length, ticks_to_wait);
  }
  this->in_play_.fetch_sub(1);
  return written;
}

void AudioRouter::start() {
  speaker::Speaker *output = this->current_();
  output->set_audio_stream_info(this->audio_stream_info_);
  output->start();
  this->state_ = speaker::STATE_RUNNING;
}

void AudioRouter::stop() {
  this->current_()->stop();
  this->state_ = speaker::STATE_STOPPED;
}

void AudioRouter::finish() {
  this->current_()->finish();
  if (this->state_ == speaker::STATE_RUNNING)
    this->state_ = speaker::STATE_STOPPING;
}

bool AudioRouter::has_buffered_data() const { return this->current_()->has_buffered_data(); }

void AudioRouter::set_pause_state(bool pause_state) {
  this->paused_ = pause_state;
  this->current_()->set_pause_state(pause_state);
}

bool AudioRouter::get_pause_state() const { return this->paused_; }

void AudioRouter::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);
  this->current_()->set_volume(volume);
}

void AudioRouter::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);
  this->current_()->set_mute_state(mute_state);
}

void AudioRouter::select_output(size_t index) {
  if (index >= this->outputs_.size())
    return;
  this->pending_ = index;
  if (index == this->active_.load() && !this->switching_.load())
    return;
  this->switching_.store(true);
  if (this->in_play_.load() == 0)
    this->switch_now_();
}

void AudioRouter::switch_now_() {
  const size_t index = this->pending_;
  speaker::Speaker *old_output = this->current_();
  const bool running = this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STARTING;
  if (index != this->active_.load()) {
    old_output->stop();
    this->active_.store(index);
    speaker::Speaker *output = this->current_();
    output->set_volume(this->volume_);
    output->set_mute_state(this->mute_state_);
    output->set_pause_state(this->paused_);
    if (running) {
      output->set_audio_stream_info(this->audio_stream_info_);
      output->start();
    } else if (this->state_ == speaker::STATE_STOPPING) {
      this->state_ = speaker::STATE_STOPPED;
    }
    uint32_t stored = index;
    this->pref_.save(&stored);
    ESP_LOGI(TAG, "Output: %s", this->output_name(index));
    this->output_callback_.call(index);
  }
  this->switching_.store(false);
}

}  // namespace esphome::audio_router

#endif  // USE_ESP32
