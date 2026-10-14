#include "audio_router.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::audio_router {

static const char *const TAG = "audio_router";

static constexpr size_t SCRATCH_BYTES = 4096;
static constexpr uint8_t MAX_CHANNELS = 8;

void AudioRouter::add_output(speaker::Speaker *output, const char *name, uint8_t bits_per_sample, uint8_t channels,
                             uint32_t sample_rate, uint32_t min_sample_rate, uint32_t max_sample_rate) {
  const size_t index = this->outputs_.size();
  this->outputs_.push_back(
      {output, name, audio::AudioStreamInfo(bits_per_sample, channels, sample_rate), min_sample_rate, max_sample_rate});
  output->add_audio_output_callback([this, index](uint32_t frames, int64_t timestamp) {
    if (this->active_.load() == index)
      this->audio_output_callback_.call(frames, timestamp);
  });
}

void AudioRouter::setup() {
  this->scratch_ = static_cast<uint8_t *>(heap_caps_malloc(SCRATCH_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (this->scratch_ == nullptr) {
    ESP_LOGE(TAG, "No memory for the %u-byte conversion buffer", static_cast<unsigned>(SCRATCH_BYTES));
    this->mark_failed();
    return;
  }
  this->pref_ = global_preferences->make_preference<uint32_t>(this->preference_key_);
  uint32_t stored = 0;
  if (this->pref_.load(&stored) && stored < this->outputs_.size())
    this->active_.store(stored);
  this->pending_ = this->active_.load();
  ESP_LOGD(TAG, "Output: %s", this->output_name(this->active_.load()));
}

void AudioRouter::loop() {
  if (this->switching_.load() && this->in_play_.load() == 0 && this->lock_.try_lock()) {
    this->switch_now_();
    this->lock_.unlock();
  }
  if (this->state_ == speaker::STATE_STOPPING && this->current_()->is_stopped())
    this->state_ = speaker::STATE_STOPPED;
}

void AudioRouter::dump_config() {
  ESP_LOGCONFIG(TAG, "Audio router:");
  for (size_t i = 0; i < this->outputs_.size(); i++) {
    const audio::AudioStreamInfo &format = this->outputs_[i].format;
    ESP_LOGCONFIG(TAG, "  Output %u: %s, %" PRIu32 " Hz (takes %" PRIu32 "-%" PRIu32 " Hz), %u bit, %u channel(s)%s",
                  static_cast<unsigned>(i), this->outputs_[i].name, format.get_sample_rate(),
                  this->outputs_[i].min_sample_rate, this->outputs_[i].max_sample_rate, format.get_bits_per_sample(),
                  format.get_channels(), i == this->active_.load() ? " (selected)" : "");
  }
}

size_t AudioRouter::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed())
    return length;
  // Players write from their own tasks, two at once when one starts while the other still ends:
  // one after the other, as the conversion keeps state between calls.
  LockGuard guard(this->lock_);
  this->in_play_.fetch_add(1);
  size_t written = 0;
  if (!this->switching_.load()) {
    // After finish() a player may still hand over what it had buffered (a resampler does): that ends
    // with it too, the router stays finishing.
    const bool finishing = this->finishing_.load();
    if (this->state_ == speaker::STATE_STOPPED) {
      this->start_output_();
      this->state_ = finishing ? speaker::STATE_STOPPING : speaker::STATE_RUNNING;
    }
    written = this->convert_ ? this->play_converted_(data, length, ticks_to_wait)
                             : this->current_()->play(data, length, ticks_to_wait);
    if (finishing)
      this->current_()->finish();
  }
  this->in_play_.fetch_sub(1);
  return written;
}

void AudioRouter::start() {
  if (this->is_failed())
    return;
  LockGuard guard(this->lock_);
  this->finishing_.store(false);
  this->start_output_();
  this->state_ = speaker::STATE_RUNNING;
}

audio::AudioStreamInfo AudioRouter::output_stream_info_() const {
  const audio::AudioStreamInfo &native = this->outputs_[this->active_.load()].format;
  return audio::AudioStreamInfo(native.get_bits_per_sample(), native.get_channels(),
                                this->audio_stream_info_.get_sample_rate());
}

void AudioRouter::start_output_() {
  this->out_info_ = this->output_stream_info_();
  const uint8_t in_bits = this->audio_stream_info_.get_bits_per_sample();
  this->convert_ = this->out_info_.get_channels() != this->audio_stream_info_.get_channels() ||
                   this->out_info_.get_bits_per_sample() != in_bits;
  this->carry_len_ = 0;
  this->carry_pos_ = 0;
  speaker::Speaker *output = this->current_();
  output->set_audio_stream_info(this->out_info_);
  output->start();
}

static inline int32_t read_sample(const uint8_t *p, uint8_t bits) {
  switch (bits) {
    case 16:
      return static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 24);
    case 24:
      return static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                                  static_cast<uint32_t>(p[2]) << 24);
    default:
      return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                                  static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24);
  }
}

static inline void write_sample(uint8_t *p, int32_t value, uint8_t bits) {
  const uint32_t v = static_cast<uint32_t>(value);
  const uint8_t bytes = bits / 8;
  for (uint8_t i = 0; i < bytes; i++)
    p[i] = static_cast<uint8_t>(v >> (32 - 8 * (bytes - i)));
}

size_t AudioRouter::play_converted_(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  speaker::Speaker *output = this->current_();
  // The rest of a frame the output took only part of goes first: the output must stay frame aligned.
  if (this->carry_pos_ < this->carry_len_) {
    this->carry_pos_ +=
        output->play(this->carry_ + this->carry_pos_, this->carry_len_ - this->carry_pos_, ticks_to_wait);
    if (this->carry_pos_ < this->carry_len_)
      return 0;
  }
  const uint8_t in_ch = this->audio_stream_info_.get_channels();
  const uint8_t in_bits = this->audio_stream_info_.get_bits_per_sample();
  const uint8_t out_ch = this->out_info_.get_channels();
  const uint8_t out_bits = this->out_info_.get_bits_per_sample();
  if (in_ch == 0 || in_ch > MAX_CHANNELS || out_ch == 0 || out_ch > MAX_CHANNELS || in_bits < 16 || in_bits > 32 ||
      in_bits % 8 != 0)
    return length;
  const size_t in_sample = in_bits / 8;
  const size_t out_sample = out_bits / 8;
  const size_t in_frame = in_ch * in_sample;
  const size_t out_frame = out_ch * out_sample;
  const size_t max_frames = SCRATCH_BYTES / out_frame;

  // Whole frames only: a player writes whole frames, and the remainder comes again with the next.
  size_t consumed = 0;
  while (length - consumed >= in_frame) {
    const size_t frames = std::min((length - consumed) / in_frame, max_frames);
    const uint8_t *src = data + consumed;
    uint8_t *dst = this->scratch_;
    int32_t in[MAX_CHANNELS];
    for (size_t f = 0; f < frames; f++, src += in_frame, dst += out_frame) {
      for (uint8_t c = 0; c < in_ch; c++)
        in[c] = read_sample(src + c * in_sample, in_bits);
      if (out_ch == 1 && in_ch > 1) {
        int64_t sum = 0;
        for (uint8_t c = 0; c < in_ch; c++)
          sum += in[c];
        write_sample(dst, static_cast<int32_t>(sum / in_ch), out_bits);
      } else {
        for (uint8_t c = 0; c < out_ch; c++)
          write_sample(dst + c * out_sample, in[c % in_ch], out_bits);
      }
    }
    const size_t bytes = frames * out_frame;
    const size_t written = output->play(this->scratch_, bytes, ticks_to_wait);
    const size_t whole = written / out_frame;
    consumed += whole * in_frame;
    if (written % out_frame != 0) {
      std::memcpy(this->carry_, this->scratch_ + whole * out_frame, out_frame);
      this->carry_len_ = out_frame;
      this->carry_pos_ = written % out_frame;
      return consumed + in_frame;
    }
    if (written < bytes)
      break;
  }
  return consumed;
}

void AudioRouter::stop() {
  this->finishing_.store(false);
  this->current_()->stop();
  this->state_ = speaker::STATE_STOPPED;
}

void AudioRouter::finish() {
  this->finishing_.store(true);
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
  if (this->in_play_.load() == 0 && this->lock_.try_lock()) {
    this->switch_now_();
    this->lock_.unlock();
  }
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
      this->start_output_();
    } else if (this->state_ == speaker::STATE_STOPPING) {
      this->state_ = speaker::STATE_STOPPED;
    }
    uint32_t stored = index;
    this->pref_.save(&stored);
    const audio::AudioStreamInfo &format = this->active_format();
    ESP_LOGI(TAG, "Output: %s (%" PRIu32 " Hz, %u bit, %u channel(s))", this->output_name(index),
             format.get_sample_rate(), format.get_bits_per_sample(), format.get_channels());
    this->output_callback_.call(index);
  }
  this->switching_.store(false);
}

}  // namespace esphome::audio_router

#endif  // USE_ESP32
