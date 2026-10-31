#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/core/static_task.h"

#include <freertos/event_groups.h>

#include <atomic>
#include <vector>

namespace esphome::resampler {

class ResamplerSpeaker final : public Component, public speaker::Speaker {
 public:
  void dump_config() override;
  void setup() override;
  void loop() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override;

  void set_pause_state(bool pause_state) override {
    if (this->output_speaker_ != nullptr)
      this->output_speaker_->set_pause_state(pause_state);
  }
  bool get_pause_state() const override {
    return this->output_speaker_ != nullptr && this->output_speaker_->get_pause_state();
  }

  bool has_buffered_data() const override;

  /// @brief Mute state changes are passed to the parent's output speaker
  void set_mute_state(bool mute_state) override;
  bool get_mute_state() override { return this->mute_state_; }

  /// @brief Volume state changes are passed to the parent's output speaker
  void set_volume(float volume) override;
  float get_volume() override { return this->volume_; }

  void set_output_speaker(speaker::Speaker *speaker) { this->output_speaker_ = speaker; }
  void add_output(speaker::Speaker *speaker, const char *name, uint32_t sample_rate, uint8_t bits_per_sample,
                  uint8_t channels) {
    this->outputs_.push_back({speaker, name, fnv1_hash(name), sample_rate, bits_per_sample, channels, this->next_order_++});
  }
  size_t add_output_group(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    this->groups_.push_back({sample_rate, bits_per_sample, channels, this->next_order_++});
    return this->groups_.size() - 1;
  }
  void add_group_output(size_t group, speaker::Speaker *speaker, const char *name, uint32_t key, uint32_t sample_rate,
                        uint8_t bits_per_sample, uint8_t channels);
  void remove_output(speaker::Speaker *speaker);
  void set_preference_key(uint32_t key) { this->preference_key_ = key; }

  void select_output(size_t index);
  size_t active_output() const { return this->active_; }
  size_t output_count() const { return this->outputs_.size(); }
  const char *output_name(size_t index) const { return this->outputs_[index].name; }
  uint32_t output_key(size_t index) const { return this->outputs_[index].key; }
  audio::AudioStreamInfo active_format() const;
  uint32_t active_min_sample_rate() const { return this->target_sample_rate_; }
  uint32_t active_max_sample_rate() const { return this->target_sample_rate_; }
  template<typename F> void add_on_output_callback(F &&callback) {
    this->output_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_outputs_changed_callback(F &&callback) {
    this->outputs_changed_callback_.add(std::forward<F>(callback));
  }
  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

  void set_target_bits_per_sample(uint8_t target_bits_per_sample) {
    this->target_bits_per_sample_ = target_bits_per_sample;
  }
  void set_target_sample_rate(uint32_t target_sample_rate) { this->target_sample_rate_ = target_sample_rate; }

  /// @brief When enabled, the input bits per sample are passed through to the output speaker unchanged instead of being
  /// converted to a fixed target. Only the sample rate is resampled if it differs from the target.
  void set_passthrough_bits_per_sample(bool passthrough_bits_per_sample) {
    this->passthrough_bits_per_sample_ = passthrough_bits_per_sample;
  }

  void set_filters(uint16_t filters) { this->filters_ = filters; }
  void set_taps(uint16_t taps) { this->taps_ = taps; }

  void set_buffer_duration(uint32_t buffer_duration_ms) { this->buffer_duration_ms_ = buffer_duration_ms; }

 protected:
  /// @brief Starts the output speaker after setting the resampled stream info. If resampling is required, it starts the
  /// task.
  /// @return ESP_OK if successful,
  ///         ESP_ERR_NO_MEM if the resampler task couldn't be created
  esp_err_t start_();

  /// @brief Transitions to STATE_STOPPING, records the stopping timestamp, sends the task stop command if the task is
  /// running, and stops the output speaker.
  void enter_stopping_state_();

  /// @brief Sets the appropriate status error based on the start failure reason.
  void set_start_error_(esp_err_t err);

  /// @brief Sends a command via event group bits, enables the loop, and optionally wakes the main loop.
  void send_command_(uint32_t command_bit, bool wake_loop = false);

  inline bool requires_resampling_() const;
  static void resample_task(void *params);

  struct Output {
    speaker::Speaker *speaker;
    const char *name;
    uint32_t key;
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
    uint8_t order;
  };
  struct Group {
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
    uint8_t order;
  };
  void apply_output_(speaker::Speaker *speaker);
  void take_pending_output_();
  void switch_to_(speaker::Speaker *speaker);
  void hook_(speaker::Speaker *speaker);
  size_t index_of_(const speaker::Speaker *speaker) const;

  std::vector<Output> outputs_;
  std::vector<Group> groups_;
  std::vector<speaker::Speaker *> hooked_;
  CallbackManager<void(size_t)> output_callback_;
  CallbackManager<void()> outputs_changed_callback_;
  ESPPreferenceObject pref_;
  uint32_t preference_key_{0};
  uint32_t wanted_key_{0};
  size_t active_{0};
  speaker::Speaker *pending_speaker_{nullptr};
  uint8_t next_order_{0};
  uint8_t output_channels_{0};
  bool switch_pending_{false};
  bool volume_set_{false};
  bool mute_set_{false};
  bool restart_after_format_{false};
  std::atomic<bool> format_changed_{false};
  audio::AudioStreamInfo started_info_;
  bool restart_after_switch_{false};

  EventGroupHandle_t event_group_{nullptr};

  std::weak_ptr<ring_buffer::RingBuffer> ring_buffer_;

  speaker::Speaker *output_speaker_{nullptr};

  StaticTask task_;

  audio::AudioStreamInfo target_stream_info_;

  uint64_t callback_remainder_{0};

  uint32_t buffer_duration_ms_;
  uint32_t state_start_ms_{0};
  uint32_t target_sample_rate_;

  uint16_t taps_;
  uint16_t filters_;

  uint8_t target_bits_per_sample_{0};

  bool passthrough_bits_per_sample_{false};
  bool task_stack_in_psram_{false};
  bool waiting_for_output_{false};
  std::atomic<bool> finishing_{false};  // finish() while the task still resamples: it ends with its input
  bool resampling_{false};              // started with the resampler task
};

}  // namespace esphome::resampler

#endif
