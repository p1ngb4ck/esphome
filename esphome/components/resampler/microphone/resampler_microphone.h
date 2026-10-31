#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/microphone/microphone_source.h"

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#ifdef USE_RESAMPLER_MICROPHONE_AEC
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/static_task.h"
#endif

#include <resampler.h>  // esp-audio-libs

#include <atomic>
#include <memory>
#include <vector>

namespace esphome::resampler {

class PositionResampler : public esp_audio_libs::resampler::Resampler {
 public:
  using Resampler::Resampler;
  float get_position() { return esp_audio_libs::art_resampler::resampleGetPosition(this->resampler_); }
};

/// @brief Microphone that converts the audio of a source microphone to a different sample rate.
/// The bits per sample and channels are selected by the source's ``MicrophoneSource``; only the sample rate changes.
/// Resampling runs in the source microphone's data callback, so it needs no task or ring buffer of its own.
/// Several sources can be configured (the same bits per sample and channels each): one of them is passed on, switched
/// at runtime (set_source), so the components using this microphone follow without knowing.
class ResamplerMicrophone final : public Component, public microphone::Microphone {
 public:
  ResamplerMicrophone() = default;
  explicit ResamplerMicrophone(microphone::MicrophoneSource *source) { this->add_source(source); }
  /// Codegen: a further source (index 1, 2, ...).
  void add_source(microphone::MicrophoneSource *source) {
    this->sources_.push_back({source, nullptr, "", 0, this->next_order_++});
  }
  void set_source_name(size_t index, const char *name) {
    this->sources_[index].name = name;
    this->sources_[index].key = fnv1_hash(name);
  }
  size_t add_source_group(uint8_t bits_per_sample, int32_t gain_factor, uint8_t channels) {
    this->groups_.push_back({bits_per_sample, gain_factor, channels, this->next_order_++});
    return this->groups_.size() - 1;
  }
  void add_group_source(size_t group, microphone::Microphone *mic, const char *name, uint32_t key);
  void remove_source(microphone::Microphone *mic);
  void set_preference_key(uint32_t key) { this->preference_key_ = key; }
#ifdef USE_RESAMPLER_MICROPHONE_AEC
  void set_source_echo_cancellation(size_t index, uint8_t mode, uint8_t reference_channel, uint8_t filter_length) {
    this->sources_[index].aec_mode = static_cast<int8_t>(mode);
    this->sources_[index].aec_reference = reference_channel;
    this->sources_[index].aec_filter = filter_length;
  }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;

  void start() override;
  void stop() override;

  void set_target_sample_rate(uint32_t target_sample_rate) { this->target_sample_rate_ = target_sample_rate; }
  void set_num_channels(uint8_t num_channels) { this->num_channels_ = num_channels; }
  void set_target_format(uint32_t sample_rate, uint8_t num_channels);
  void set_filters(uint16_t filters) { this->filters_ = filters; }
  void set_taps(uint16_t taps) { this->taps_ = taps; }

  /// Main loop: the source passed on from now on (an index of the configured list), -1 (or out of range): none, no
  /// audio. While capturing, the old source is stopped and the new one started.
  void set_source(int index);
  int get_source() const;
  size_t source_count() const { return this->sources_.size(); }
  microphone::MicrophoneSource *source_at(size_t index) const {
    return index < this->sources_.size() ? this->sources_[index].source : nullptr;
  }
  const char *source_name(size_t index) const { return this->sources_[index].name; }
  microphone::Microphone *source_microphone(size_t index) const {
    return index < this->sources_.size() ? this->sources_[index].mic : nullptr;
  }
  template<typename F> void add_on_source_callback(F &&callback) { this->source_callback_.add(std::forward<F>(callback)); }
  template<typename F> void add_on_sources_changed_callback(F &&callback) {
    this->sources_changed_callback_.add(std::forward<F>(callback));
  }

 protected:
  /// @brief Sets up the resampler for the given input format. No resampler is needed if the sample rates match.
  /// @return false if the resampler failed to allocate; the audio is then dropped
  bool init_resampler_(const audio::AudioStreamInfo &input_stream_info);
  uint8_t output_channels_(uint8_t input_channels) const {
    return this->num_channels_ != 0 ? this->num_channels_ : input_channels;
  }

  /// @brief Resamples a chunk of source audio and passes it to the data callbacks. Source microphone task only.
  void process_audio_(microphone::MicrophoneSource *source, std::span<const uint8_t> data, int64_t capture_time_us);
  void process_chunk_(microphone::MicrophoneSource *source, std::span<const uint8_t> data, int64_t capture_time_us);
  void emit_(const audio::AudioStreamInfo &input_stream_info, const uint8_t *data, size_t len, bool from_source,
             int64_t capture_time_us);
  void publish_source_(const uint8_t *data, size_t len, int64_t capture_time_us);
#ifdef USE_RESAMPLER_MICROPHONE_AEC
  bool aec_wanted_() const;
  bool aec_update_();
  void aec_input_(const audio::AudioStreamInfo &input_stream_info, std::span<const uint8_t> data,
                  int64_t capture_time_us);
  void aec_release_();
  static void aec_task_entry_(void *param);
  void aec_run_();
#endif
  microphone::MicrophoneSource *active_source_() const { return this->active_.load(); }
  void hook_(microphone::MicrophoneSource *source);
  void switch_source_(microphone::MicrophoneSource *source);
  size_t index_of_(const microphone::MicrophoneSource *source) const;

  struct Source {
    microphone::MicrophoneSource *source;
    microphone::Microphone *mic;
    const char *name;
    uint32_t key;
    uint8_t order;
    int8_t aec_mode{-1};
    uint8_t aec_reference{1};
    uint8_t aec_filter{4};
  };
  struct Group {
    uint8_t bits_per_sample;
    int32_t gain_factor;
    uint8_t channels;
    uint8_t order;
  };

  std::vector<Source> sources_;
  std::vector<Group> groups_;
  std::vector<Source> made_;
  std::atomic<microphone::MicrophoneSource *> active_{nullptr};
  CallbackManager<void(int)> source_callback_;
  CallbackManager<void()> sources_changed_callback_;
  ESPPreferenceObject pref_;
  uint32_t preference_key_{0};
  uint32_t wanted_key_{0};
  bool wanted_none_{false};
  uint8_t next_order_{0};
  // A switch can let the old source's task and the new one's overlap for one chunk: the second is dropped
  std::atomic<bool> processing_{false};
  std::atomic<bool> emitting_{false};
  std::unique_ptr<PositionResampler> resampler_;
  // Reused for every chunk so resampling does not allocate
  std::vector<uint8_t> output_buffer_;

  // Format the resampler is set up for
  audio::AudioStreamInfo input_stream_info_;

  uint32_t target_sample_rate_;
  uint32_t pending_sample_rate_{0};
  std::atomic<bool> format_pending_{false};
  uint16_t taps_;
  uint16_t filters_;

  uint8_t active_listeners_{0};
  uint8_t num_channels_{0};
  uint8_t pending_num_channels_{0};
  bool resampler_ready_{false};
#ifdef USE_RESAMPLER_MICROPHONE_AEC
  void *aec_{nullptr};
  std::atomic<microphone::MicrophoneSource *> aec_source_{nullptr};
  StaticTask aec_task_;
  std::unique_ptr<ring_buffer::RingBuffer> aec_ring_;
  int16_t *aec_in_{nullptr};
  int16_t *aec_mic_{nullptr};
  int16_t *aec_ref_{nullptr};
  int16_t *aec_out_{nullptr};
  size_t aec_frame_{0};
  uint8_t aec_reference_{1};
  bool aec_failed_{false};
  bool aec_stopping_{false};
  std::atomic<bool> aec_stop_{false};
  std::atomic<bool> aec_parked_{false};
#endif
};

}  // namespace esphome::resampler

#endif  // USE_ESP32
