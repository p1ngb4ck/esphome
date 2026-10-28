#include "resampler_microphone.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <new>

#ifdef USE_RESAMPLER_MICROPHONE_AEC
#include "esphome/core/hal.h"

#include <esp_aec.h>
#include <esp_heap_caps.h>
#endif

namespace esphome::resampler {

ESPHOME_LOG_TAG(TAG, "resampler.microphone");

// Duration of audio the resampler converts per step; longer source chunks are processed in several steps
static constexpr uint32_t BUFFER_DURATION_MS = 16;
#ifdef USE_RESAMPLER_MICROPHONE_AEC
static constexpr uint32_t AEC_TASK_STACK_SIZE = 8192;
static constexpr UBaseType_t AEC_TASK_PRIORITY = 18;
#endif

void ResamplerMicrophone::setup() {
  if (!this->sources_.empty()) {
    const audio::AudioStreamInfo input_stream_info = this->sources_[0].source->get_audio_stream_info();
    this->audio_stream_info_ = audio::AudioStreamInfo(input_stream_info.get_bits_per_sample(),
                                                      this->output_channels_(input_stream_info.get_channels()),
                                                      this->target_sample_rate_);

    // Allocate now for the expected source format; process_audio_ only sets up again if that format changes
    if (!this->init_resampler_(input_stream_info)) {
      this->mark_failed();
      return;
    }
  } else if (!this->groups_.empty()) {
    this->audio_stream_info_ =
        audio::AudioStreamInfo(this->groups_[0].bits_per_sample,
                               this->output_channels_(__builtin_popcount(this->groups_[0].channels)),
                               this->target_sample_rate_);
  }

  if (this->preference_key_ != 0) {
    this->pref_ = global_preferences->make_preference<uint32_t>(this->preference_key_);
    uint32_t stored = 0;
    if (this->pref_.load(&stored))
      this->wanted_key_ = stored;
  }
  microphone::MicrophoneSource *initial = this->sources_.empty() ? nullptr : this->sources_[0].source;
  for (const Source &source : this->sources_) {
    this->hook_(source.source);
    if (this->wanted_key_ != 0 && source.key == this->wanted_key_)
      initial = source.source;
  }
  this->active_.store(initial);

  this->disable_loop();
}

void ResamplerMicrophone::hook_(microphone::MicrophoneSource *source) {
  source->add_data_callback([this, source](const microphone::AudioCapture &capture) {
    if (this->active_.load() == source)
      this->process_audio_(source, capture.data);
  });
}

size_t ResamplerMicrophone::index_of_(const microphone::MicrophoneSource *source) const {
  for (size_t i = 0; i < this->sources_.size(); i++) {
    if (this->sources_[i].source == source)
      return i;
  }
  return this->sources_.size();
}

int ResamplerMicrophone::get_source() const {
  const size_t index = this->index_of_(this->active_.load());
  return index < this->sources_.size() ? static_cast<int>(index) : -1;
}

void ResamplerMicrophone::add_group_source(size_t group, microphone::Microphone *mic, const char *name, uint32_t key) {
  if (group >= this->groups_.size())
    return;
  const Group &g = this->groups_[group];
  microphone::MicrophoneSource *source = nullptr;
  for (const Source &made : this->made_) {
    if (made.mic == mic && made.order == g.order)
      source = made.source;
  }
  if (source == nullptr) {
    RAMAllocator<microphone::MicrophoneSource> allocator(RAMAllocator<microphone::MicrophoneSource>::ALLOC_EXTERNAL);
    microphone::MicrophoneSource *mem = allocator.allocate(1);
    if (mem == nullptr) {
      ESP_LOGE(TAG, "No PSRAM for source %s", name);
      return;
    }
    source = new (mem) microphone::MicrophoneSource(mic, g.bits_per_sample, g.gain_factor, false);
    for (uint8_t channel = 0; channel < 8; channel++) {
      if (g.channels & (1u << channel))
        source->add_channel(channel);
    }
    this->hook_(source);
    this->made_.push_back({source, mic, name, key, g.order});
  }
  if (this->index_of_(source) != this->sources_.size())
    return;
  size_t position = 0;
  while (position < this->sources_.size() && this->sources_[position].order <= g.order)
    position++;
  this->sources_.insert(this->sources_.begin() + position, {source, mic, name, key, g.order});
  ESP_LOGI(TAG, "Source added: %s", name);
  if (!this->wanted_none_ &&
      (key == this->wanted_key_ || (this->active_.load() == nullptr && this->sources_.size() == 1)))
    this->switch_source_(source);
  this->sources_changed_callback_.call();
}

void ResamplerMicrophone::remove_source(microphone::Microphone *mic) {
  size_t index = 0;
  while (index < this->sources_.size() && this->sources_[index].mic != mic)
    index++;
  if (index == this->sources_.size() || mic == nullptr)
    return;
  microphone::MicrophoneSource *source = this->sources_[index].source;
  ESP_LOGI(TAG, "Source gone: %s", this->sources_[index].name);
  this->sources_.erase(this->sources_.begin() + index);
  if (this->active_.load() == source)
    this->switch_source_(this->sources_.empty() ? nullptr : this->sources_[0].source);
  this->sources_changed_callback_.call();
}

void ResamplerMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Resampler Microphone:\n"
                "  Target Sample Rate: %" PRIu32 " Hz\n"
                "  Taps: %u\n"
                "  Filters: %u\n"
                "  Sources: %u, active %d",
                this->target_sample_rate_, this->taps_, this->filters_, static_cast<unsigned>(this->sources_.size()),
                this->get_source());
}

void ResamplerMicrophone::set_source(int index) {
  microphone::MicrophoneSource *next = nullptr;
  if (index >= 0 && static_cast<size_t>(index) < this->sources_.size()) {
    next = this->sources_[index].source;
    this->wanted_key_ = this->sources_[index].key;
    this->wanted_none_ = false;
  } else {
    this->wanted_key_ = 0;
    this->wanted_none_ = true;
  }
  if (this->preference_key_ != 0)
    this->pref_.save(&this->wanted_key_);
  this->switch_source_(next);
}

void ResamplerMicrophone::switch_source_(microphone::MicrophoneSource *next) {
  microphone::MicrophoneSource *old = this->active_.load();
  if (next == old)
    return;
  // Nothing of the old one passed on from here; the new one's first chunk sets the resampler up for its rate
  this->active_.store(nullptr);
  if (this->state_ != microphone::STATE_STOPPED) {
    if (old != nullptr)
      old->stop();
    if (next != nullptr)
      next->start();
    this->state_ = microphone::STATE_STARTING;
  }
  this->active_.store(next);
  ESP_LOGD(TAG, "Source %d", this->get_source());
  this->source_callback_.call(this->get_source());
  this->enable_loop();
}

void ResamplerMicrophone::set_target_format(uint32_t sample_rate, uint8_t num_channels) {
  this->pending_sample_rate_ = sample_rate;
  this->pending_num_channels_ = num_channels;
  this->audio_stream_info_ =
      audio::AudioStreamInfo(this->audio_stream_info_.get_bits_per_sample(),
                             num_channels != 0 ? num_channels : this->audio_stream_info_.get_channels(), sample_rate);
  this->format_pending_.store(true);
}

void ResamplerMicrophone::start() {
  if (this->is_failed() || this->active_listeners_ == UINT8_MAX)
    return;
  ++this->active_listeners_;
  this->enable_loop();
}

void ResamplerMicrophone::stop() {
  if (this->active_listeners_ == 0)
    return;
  --this->active_listeners_;
  this->enable_loop();
}

void ResamplerMicrophone::loop() {
  microphone::MicrophoneSource *source = this->active_source_();
  if (this->active_listeners_ == 0) {
    if (this->state_ != microphone::STATE_STOPPED) {
      if (source != nullptr)
        source->stop();
      this->state_ = microphone::STATE_STOPPED;
    }
#ifdef USE_RESAMPLER_MICROPHONE_AEC
    if (!this->aec_update_())
      return;
#endif
    this->disable_loop();
    return;
  }
#ifdef USE_RESAMPLER_MICROPHONE_AEC
  this->aec_update_();
#endif

  switch (this->state_) {
    case microphone::STATE_STOPPED:
      if (source != nullptr)
        source->start();
      this->state_ = microphone::STATE_STARTING;
      break;
    case microphone::STATE_STARTING:
      // No source: running without audio (the users wait for no microphone)
      if (source == nullptr || source->is_running()) {
        this->state_ = microphone::STATE_RUNNING;
      }
      break;
    case microphone::STATE_RUNNING:
      // Follow the source if it restarts, e.g. after a driver error
      if (source != nullptr && !source->is_running()) {
        this->state_ = microphone::STATE_STARTING;
      }
      break;
    case microphone::STATE_STOPPING:
      break;
  }
}

bool ResamplerMicrophone::init_resampler_(const audio::AudioStreamInfo &input_stream_info) {
  this->resampler_.reset();
  this->resampler_ready_ = false;

  if (input_stream_info.get_sample_rate() == this->target_sample_rate_) {
    // The source already delivers the target sample rate, so its audio is passed through unchanged
    this->input_stream_info_ = input_stream_info;
    this->resampler_ready_ = true;
    return true;
  }

  const audio::AudioStreamInfo output_stream_info(input_stream_info.get_bits_per_sample(),
                                                  input_stream_info.get_channels(), this->target_sample_rate_);

  auto resampler = make_unique<esp_audio_libs::resampler::Resampler>(
      input_stream_info.ms_to_samples(BUFFER_DURATION_MS), output_stream_info.ms_to_samples(BUFFER_DURATION_MS));

  esp_audio_libs::resampler::ResamplerConfiguration resample_config = {
      .source_sample_rate = static_cast<float>(input_stream_info.get_sample_rate()),
      .target_sample_rate = static_cast<float>(this->target_sample_rate_),
      .source_bits_per_sample = input_stream_info.get_bits_per_sample(),
      .target_bits_per_sample = input_stream_info.get_bits_per_sample(),
      .channels = input_stream_info.get_channels(),
      // Filters out frequencies above the new Nyquist limit when downsampling, to avoid aliasing
      .use_pre_or_post_filter = this->target_sample_rate_ < input_stream_info.get_sample_rate(),
      .subsample_interpolate = false,  // Doubles the CPU load; more filters is a better alternative
      .number_of_taps = this->taps_,
      .number_of_filters = this->filters_,
  };

  if (!resampler->initialize(resample_config)) {
    ESP_LOGE(TAG, "Not enough memory to resample");
    return false;
  }

  const audio::AudioStreamInfo widest_stream_info(
      input_stream_info.get_bits_per_sample(),
      std::max(input_stream_info.get_channels(), this->output_channels_(input_stream_info.get_channels())),
      this->target_sample_rate_);
  this->output_buffer_.reserve(widest_stream_info.ms_to_bytes(BUFFER_DURATION_MS));
  this->resampler_ = std::move(resampler);
  // Only set on success, so a failed set up is retried with the next chunk
  this->input_stream_info_ = input_stream_info;
  this->resampler_ready_ = true;
  return true;
}

void ResamplerMicrophone::process_audio_(microphone::MicrophoneSource *source, std::span<const uint8_t> data) {
  if (this->processing_.exchange(true))
    return;
  this->process_chunk_(source, data);
  this->processing_.store(false);
}

void ResamplerMicrophone::process_chunk_(microphone::MicrophoneSource *source, std::span<const uint8_t> data) {
  const audio::AudioStreamInfo input_stream_info = source->get_audio_stream_info();
#ifdef USE_RESAMPLER_MICROPHONE_AEC
  if (source == this->aec_source_.load()) {
    this->aec_input_(input_stream_info, data);
    return;
  }
#endif
  if (this->emitting_.exchange(true))
    return;
  this->emit_(input_stream_info, data.data(), data.size(), true);
  this->emitting_.store(false);
}

void ResamplerMicrophone::publish_source_(const uint8_t *data, size_t len) {
  if (this->mute_state_) {
    this->output_buffer_.assign(len, 0);
    this->publish_audio_(this->output_buffer_, this->audio_stream_info_);
    return;
  }
  this->data_callbacks_.call(microphone::AudioCapture{std::span<const uint8_t>(data, len), this->audio_stream_info_, 0});
}

void ResamplerMicrophone::emit_(const audio::AudioStreamInfo &input_stream_info, const uint8_t *data, size_t len,
                                bool from_source) {
  bool reinit = false;
  if (this->format_pending_.exchange(false)) {
    this->target_sample_rate_ = this->pending_sample_rate_;
    this->num_channels_ = this->pending_num_channels_;
    reinit = true;
  }
  if (reinit || input_stream_info != this->input_stream_info_) {
    this->init_resampler_(input_stream_info);
  }
  if (!this->resampler_ready_) {
    return;
  }

  const uint8_t in_channels = input_stream_info.get_channels();
  const uint8_t out_channels = this->output_channels_(in_channels);
  const audio::AudioStreamInfo resampled_stream_info(input_stream_info.get_bits_per_sample(), in_channels,
                                                     this->target_sample_rate_);
  const audio::AudioStreamInfo output_stream_info(input_stream_info.get_bits_per_sample(), out_channels,
                                                  this->target_sample_rate_);
  const size_t sample_bytes = input_stream_info.samples_to_bytes(1);

  if (this->resampler_ == nullptr) {
    if (in_channels == out_channels && from_source) {
      this->publish_source_(data, len);
      return;
    }
    const uint32_t frames = input_stream_info.bytes_to_frames(len);
    this->output_buffer_.resize(output_stream_info.frames_to_bytes(frames));
    audio::convert_channels(data, this->output_buffer_.data(), frames, in_channels, out_channels, sample_bytes);
    this->publish_audio_(this->output_buffer_, this->audio_stream_info_);
    return;
  }

  const size_t input_bytes_per_frame = input_stream_info.frames_to_bytes(1);
  const uint32_t max_input_frames = input_stream_info.ms_to_frames(BUFFER_DURATION_MS);
  // Both limits match the sizes the resampler's internal buffers were allocated with in init_resampler_
  const uint32_t max_output_frames = resampled_stream_info.ms_to_frames(BUFFER_DURATION_MS);
  const size_t widest_frame_bytes = sample_bytes * std::max(in_channels, out_channels);

  const uint8_t *input = data;
  uint32_t input_frames = input_stream_info.bytes_to_frames(len);
  while (input_frames > 0) {
    // Stays within the reserved capacity, so this never reallocates
    this->output_buffer_.resize(max_output_frames * widest_frame_bytes);

    // The resampler's internal buffers hold at most BUFFER_DURATION_MS of audio, so feed it in steps of that size.
    // 0 dB keeps the microphone level that downstream detectors are tuned for; overshoot saturates instead of wrapping.
    esp_audio_libs::resampler::ResamplerResults results = this->resampler_->resample(
        input, this->output_buffer_.data(), std::min(input_frames, max_input_frames), max_output_frames, 0.0f);

    input += results.frames_used * input_bytes_per_frame;
    input_frames -= results.frames_used;

    if (results.frames_generated > 0) {
      if (in_channels != out_channels) {
        audio::convert_channels(this->output_buffer_.data(), this->output_buffer_.data(), results.frames_generated,
                                in_channels, out_channels, sample_bytes);
      }
      this->output_buffer_.resize(output_stream_info.frames_to_bytes(results.frames_generated));
      this->publish_audio_(this->output_buffer_, this->audio_stream_info_);
    } else if (results.frames_used == 0) {
      break;  // No progress; drop the rest of the chunk instead of spinning
    }
  }
}

#ifdef USE_RESAMPLER_MICROPHONE_AEC
bool ResamplerMicrophone::aec_wanted_() const {
  if (this->active_listeners_ == 0 || this->state_ == microphone::STATE_STOPPED || this->aec_failed_)
    return false;
  const size_t index = this->index_of_(this->active_.load());
  return index < this->sources_.size() && this->sources_[index].aec_mode >= 0;
}

void ResamplerMicrophone::aec_release_() {
  aec_destroy(static_cast<aec_handle_t *>(this->aec_));
  this->aec_ = nullptr;
  this->aec_ring_.reset();
  heap_caps_free(this->aec_in_);
  heap_caps_free(this->aec_mic_);
  heap_caps_free(this->aec_ref_);
  heap_caps_free(this->aec_out_);
  this->aec_in_ = this->aec_mic_ = this->aec_ref_ = this->aec_out_ = nullptr;
  this->aec_stopping_ = false;
  this->aec_stop_.store(false);
  this->aec_parked_.store(false);
}

bool ResamplerMicrophone::aec_update_() {
  microphone::MicrophoneSource *active = this->active_.load();
  if (this->active_listeners_ == 0)
    this->aec_failed_ = false;
  const bool wanted = this->aec_wanted_();
  if (this->aec_ != nullptr && (this->aec_stopping_ || !wanted || this->aec_source_.load() != active)) {
    if (!this->aec_stopping_) {
      if (this->processing_.exchange(true))
        return false;
      this->aec_source_.store(nullptr);
      this->processing_.store(false);
      this->aec_stopping_ = true;
      this->aec_stop_.store(true);
      xTaskNotifyGive(this->aec_task_.get_handle());
    }
    if (!this->aec_parked_.load() || !this->aec_task_.deallocate())
      return false;
    this->aec_release_();
    ESP_LOGD(TAG, "Echo cancellation unloaded");
  }
  if (!wanted || this->aec_ != nullptr)
    return true;
  const Source &source = this->sources_[this->index_of_(active)];
  aec_handle_t *handle = aec_create(16000, source.aec_filter, 1, static_cast<aec_mode_t>(source.aec_mode));
  if (handle == nullptr) {
    ESP_LOGE(TAG, "No memory for echo cancellation");
    this->aec_failed_ = true;
    return true;
  }
  this->aec_ = handle;
  this->aec_frame_ = static_cast<size_t>(aec_get_chunksize(handle));
  const size_t bytes = this->aec_frame_ * sizeof(int16_t);
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  this->aec_in_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, 2 * bytes, caps));
  this->aec_mic_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, caps));
  this->aec_ref_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, caps));
  this->aec_out_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, caps));
  this->aec_ring_ = ring_buffer::RingBuffer::create(16000 * 2 * sizeof(int16_t) / 10);
  this->aec_reference_ = source.aec_reference;
  this->aec_stop_.store(false);
  this->aec_parked_.store(false);
  if (this->aec_in_ == nullptr || this->aec_mic_ == nullptr || this->aec_ref_ == nullptr ||
      this->aec_out_ == nullptr || this->aec_ring_ == nullptr ||
      !this->aec_task_.create(aec_task_entry_, "mic_aec", AEC_TASK_STACK_SIZE, this, AEC_TASK_PRIORITY, false,
                              LOOP_TASK_CORE)) {
    ESP_LOGE(TAG, "No memory for echo cancellation");
    this->aec_release_();
    this->aec_failed_ = true;
    return true;
  }
  this->aec_source_.store(active);
  ESP_LOGD(TAG, "Echo cancellation loaded (%u samples per frame)", static_cast<unsigned>(this->aec_frame_));
  return true;
}

void ResamplerMicrophone::aec_input_(const audio::AudioStreamInfo &input_stream_info,
                                     std::span<const uint8_t> data) {
  if (input_stream_info.get_sample_rate() != 16000 || input_stream_info.get_bits_per_sample() != 16 ||
      input_stream_info.get_channels() != 2) {
    if (this->emitting_.exchange(true))
      return;
    this->emit_(input_stream_info, data.data(), data.size(), true);
    this->emitting_.store(false);
    return;
  }
  this->aec_ring_->write(data.data(), data.size());
  xTaskNotifyGive(this->aec_task_.get_handle());
}

void ResamplerMicrophone::aec_task_entry_(void *param) { static_cast<ResamplerMicrophone *>(param)->aec_run_(); }

void ResamplerMicrophone::aec_run_() {
  const audio::AudioStreamInfo mono_stream_info(16, 1, 16000);
  const size_t frame_bytes = this->aec_frame_ * 2 * sizeof(int16_t);
  const uint8_t ref = this->aec_reference_ & 1;
  while (!this->aec_stop_.load()) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!this->aec_stop_.load() && this->aec_ring_->available() >= frame_bytes) {
      this->aec_ring_->read(this->aec_in_, frame_bytes, 0);
      for (size_t i = 0; i < this->aec_frame_; i++) {
        this->aec_mic_[i] = this->aec_in_[2 * i + (1 - ref)];
        this->aec_ref_[i] = this->aec_in_[2 * i + ref];
      }
      aec_process(static_cast<const aec_handle_t *>(this->aec_), this->aec_mic_, this->aec_ref_, this->aec_out_);
      if (this->emitting_.exchange(true))
        continue;
      this->emit_(mono_stream_info, reinterpret_cast<const uint8_t *>(this->aec_out_),
                  this->aec_frame_ * sizeof(int16_t), false);
      this->emitting_.store(false);
    }
  }
  this->aec_parked_.store(true);
  this->enable_loop_soon_any_context();
  vTaskSuspend(nullptr);
}
#endif

}  // namespace esphome::resampler

#endif  // USE_ESP32
