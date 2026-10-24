#include "resampler_speaker.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio_resampler.h"

#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::resampler {

// Above the main loop: paced by its input and output, it takes no more than the sound needs.
static const UBaseType_t RESAMPLER_TASK_PRIORITY = 2;

static const uint32_t TRANSFER_BUFFER_DURATION_MS = 50;

static const uint32_t TASK_STACK_SIZE = 3072;

static const uint32_t STATE_TRANSITION_TIMEOUT_MS = 5000;

ESPHOME_LOG_TAG(TAG, "resampler_speaker");

enum ResamplingEventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),       // signals stop request
  COMMAND_START = (1 << 1),      // signals start request
  COMMAND_FINISH = (1 << 2),     // signals finish request (graceful stop)
  TASK_COMMAND_STOP = (1 << 5),  // signals the task to stop
  TASK_COMMAND_FINISH = (1 << 6),  // signals the task to end once its input is used up
  STATE_STARTING = (1 << 10),
  STATE_RUNNING = (1 << 11),
  STATE_STOPPING = (1 << 12),
  STATE_STOPPED = (1 << 13),
  ERR_ESP_NO_MEM = (1 << 19),
  ERR_ESP_NOT_SUPPORTED = (1 << 20),
  ERR_ESP_FAIL = (1 << 21),
  ALL_BITS = 0x00FFFFFF,  // All valid FreeRTOS event group bits
};

void ResamplerSpeaker::dump_config() {
  if (this->passthrough_bits_per_sample_) {
    ESP_LOGCONFIG(TAG,
                  "Resampler Speaker:\n"
                  "  Target Bits Per Sample: passthrough\n"
                  "  Target Sample Rate: %" PRIu32 " Hz",
                  this->target_sample_rate_);
  } else {
    ESP_LOGCONFIG(TAG,
                  "Resampler Speaker:\n"
                  "  Target Bits Per Sample: %" PRIu8 "\n"
                  "  Target Sample Rate: %" PRIu32 " Hz",
                  this->target_bits_per_sample_, this->target_sample_rate_);
  }
}

void ResamplerSpeaker::setup() {
  this->event_group_ = xEventGroupCreate();
  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }

  if (this->outputs_.empty() && this->groups_.empty()) {
    this->outputs_.push_back({this->output_speaker_, "", 0, this->target_sample_rate_,
                              this->passthrough_bits_per_sample_ ? static_cast<uint8_t>(0) : this->target_bits_per_sample_,
                              0, 0});
  }
  if (this->preference_key_ != 0 && this->outputs_.size() + this->groups_.size() > 1) {
    this->pref_ = global_preferences->make_preference<uint32_t>(this->preference_key_);
    uint32_t stored = 0;
    if (this->pref_.load(&stored))
      this->wanted_key_ = stored;
  }
  this->output_speaker_ = nullptr;
  if (!this->outputs_.empty()) {
    size_t index = 0;
    for (size_t i = 0; i < this->outputs_.size(); i++) {
      if (this->outputs_[i].key == this->wanted_key_)
        index = i;
    }
    this->apply_output_(this->outputs_[index].speaker);
  }
  this->pending_speaker_ = this->output_speaker_;

  for (const Output &output : this->outputs_)
    this->hook_(output.speaker);

  // Start with loop disabled since no task is running and no commands are pending
  this->disable_loop();
}

void ResamplerSpeaker::hook_(speaker::Speaker *speaker) {
  if (std::find(this->hooked_.begin(), this->hooked_.end(), speaker) != this->hooked_.end())
    return;
  this->hooked_.push_back(speaker);
  speaker->add_audio_output_callback([this, speaker](uint32_t new_frames, int64_t write_timestamp) {
    if (this->output_speaker_ != speaker)
      return;
    if (this->audio_stream_info_.get_sample_rate() != this->target_stream_info_.get_sample_rate()) {
      // Convert the number of frames from the target sample rate to the source sample rate. Track the remainder to
      // avoid losing frames from integer division truncation.
      const uint64_t numerator = new_frames * this->audio_stream_info_.get_sample_rate() + this->callback_remainder_;
      const uint64_t denominator = this->target_stream_info_.get_sample_rate();
      this->callback_remainder_ = numerator % denominator;
      this->audio_output_callback_(numerator / denominator, write_timestamp);
    } else {
      this->audio_output_callback_(new_frames, write_timestamp);
    }
  });
}

size_t ResamplerSpeaker::index_of_(const speaker::Speaker *speaker) const {
  for (size_t i = 0; i < this->outputs_.size(); i++) {
    if (this->outputs_[i].speaker == speaker)
      return i;
  }
  return this->outputs_.size();
}

void ResamplerSpeaker::add_group_output(size_t group, speaker::Speaker *speaker, const char *name, uint32_t key,
                                        uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
  if (group >= this->groups_.size() || this->index_of_(speaker) != this->outputs_.size())
    return;
  const Group &g = this->groups_[group];
  size_t position = 0;
  while (position < this->outputs_.size() && this->outputs_[position].order <= g.order)
    position++;
  this->outputs_.insert(this->outputs_.begin() + position,
                        {speaker, name, key, sample_rate, bits_per_sample, channels, g.order});
  this->hook_(speaker);
  ESP_LOGI(TAG, "Output added: %s", name);
  if (this->output_speaker_ == nullptr) {
    this->apply_output_(speaker);
    this->pending_speaker_ = speaker;
    this->output_callback_.call(this->active_);
  } else {
    this->active_ = this->index_of_(this->output_speaker_);
    if (key == this->wanted_key_ && this->output_speaker_ != speaker)
      this->switch_to_(speaker);
  }
  this->outputs_changed_callback_.call();
}

void ResamplerSpeaker::remove_output(speaker::Speaker *speaker) {
  const size_t index = this->index_of_(speaker);
  if (index == this->outputs_.size())
    return;
  ESP_LOGI(TAG, "Output gone: %s", this->outputs_[index].name);
  this->outputs_.erase(this->outputs_.begin() + index);
  if (this->output_speaker_ == speaker) {
    speaker::Speaker *fallback = this->outputs_.empty() ? nullptr : this->outputs_[0].speaker;
    this->pending_speaker_ = fallback;
    this->switch_pending_ = true;
    if (this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STARTING) {
      this->restart_after_switch_ = fallback != nullptr;
      this->waiting_for_output_ = false;
      this->enter_stopping_state_();
    } else if (this->state_ == speaker::STATE_STOPPED) {
      this->take_pending_output_();
    }
    this->enable_loop();
  } else {
    if (this->switch_pending_ && this->pending_speaker_ == speaker)
      this->pending_speaker_ = this->output_speaker_;
    this->active_ = this->index_of_(this->output_speaker_);
  }
  this->outputs_changed_callback_.call();
}

void ResamplerSpeaker::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  // Process commands with priority: STOP > FINISH > START
  // This ensures stop commands take precedence over conflicting start commands
  if (event_group_bits & ResamplingEventGroupBits::COMMAND_STOP) {
    if (this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STARTING) {
      // Clear STOP, START, and FINISH bits - stop takes precedence
      xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::COMMAND_STOP |
                                                   ResamplingEventGroupBits::COMMAND_START |
                                                   ResamplingEventGroupBits::COMMAND_FINISH);
      this->waiting_for_output_ = false;
      this->restart_after_format_ = false;
      this->format_changed_.store(false);
      this->enter_stopping_state_();
    } else if (this->state_ == speaker::STATE_STOPPED) {
      // Already stopped, just clear the command bits
      xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::COMMAND_STOP |
                                                   ResamplingEventGroupBits::COMMAND_START |
                                                   ResamplingEventGroupBits::COMMAND_FINISH);
    }
    // Leave bits set if STATE_STOPPING - will be processed once stopped
  } else if (event_group_bits & ResamplingEventGroupBits::COMMAND_FINISH) {
    if (this->state_ == speaker::STATE_RUNNING) {
      xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::COMMAND_FINISH);
      // The input ended (the end of a file): over once it is all handed on, not once the output is quiet.
      if (this->task_.is_created()) {
        this->finishing_.store(true);
        xEventGroupSetBits(this->event_group_, ResamplingEventGroupBits::TASK_COMMAND_FINISH);
      } else {
        this->output_speaker_->finish();
        this->state_ = speaker::STATE_STOPPED;
      }
    } else if (this->state_ == speaker::STATE_STOPPED) {
      // Already stopped, just clear the command bit
      xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::COMMAND_FINISH);
    }
    // Leave bit set if transitioning states - will be processed once state allows
  } else if (event_group_bits & ResamplingEventGroupBits::COMMAND_START) {
    if (this->state_ == speaker::STATE_STOPPED) {
      xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::COMMAND_START);
      this->state_ = speaker::STATE_STARTING;
    } else if (this->state_ == speaker::STATE_RUNNING) {
      // Already running, just clear the command bit
      xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::COMMAND_START);
    }
    // Leave bit set if transitioning states - will be processed once state allows
  }

  // Re-read bits after command processing (enter_stopping_state_ may have set task bits)
  event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & ResamplingEventGroupBits::STATE_STARTING) {
    ESP_LOGD(TAG, "Starting");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::STATE_STARTING);
  }

  if (event_group_bits & ResamplingEventGroupBits::ERR_ESP_NO_MEM) {
    this->status_set_error(LOG_STR("Not enough memory"));
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ERR_ESP_NO_MEM);
    this->enter_stopping_state_();
  }
  if (event_group_bits & ResamplingEventGroupBits::ERR_ESP_NOT_SUPPORTED) {
    this->status_set_error(LOG_STR("Unsupported stream"));
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ERR_ESP_NOT_SUPPORTED);
    this->enter_stopping_state_();
  }
  if (event_group_bits & ResamplingEventGroupBits::ERR_ESP_FAIL) {
    this->status_set_error(LOG_STR("Resampler failure"));
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ERR_ESP_FAIL);
    this->enter_stopping_state_();
  }

  if (event_group_bits & ResamplingEventGroupBits::STATE_RUNNING) {
    ESP_LOGV(TAG, "Started");
    this->status_clear_error();
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::STATE_RUNNING);
  }
  if (event_group_bits & ResamplingEventGroupBits::STATE_STOPPING) {
    ESP_LOGV(TAG, "Stopping");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::STATE_STOPPING);
  }
  // Retries on a subsequent loop if the task is still running on the other core
  if ((event_group_bits & ResamplingEventGroupBits::STATE_STOPPED) && this->task_.deallocate()) {
    ESP_LOGD(TAG, "Stopped");
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::ALL_BITS);
  }

  switch (this->state_) {
    case speaker::STATE_STARTING: {
      if (!this->waiting_for_output_) {
        if (this->switch_pending_) {
          this->take_pending_output_();
          this->restart_after_switch_ = false;
        }
        if (this->output_speaker_ == nullptr) {
          this->state_ = speaker::STATE_STOPPED;
          break;
        }
        esp_err_t err = this->start_();
        if (err == ESP_OK) {
          this->finishing_.store(false);
          this->callback_remainder_ = 0;  // reset callback remainder
          this->status_clear_error();
          this->waiting_for_output_ = true;
          this->state_start_ms_ = App.get_loop_component_start_time();
        } else {
          this->set_start_error_(err);
          this->waiting_for_output_ = false;
          this->enter_stopping_state_();
        }
      } else {
        if (this->output_speaker_->is_running()) {
          this->state_ = speaker::STATE_RUNNING;
          this->waiting_for_output_ = false;
        } else if ((App.get_loop_component_start_time() - this->state_start_ms_) > STATE_TRANSITION_TIMEOUT_MS) {
          // Timed out waiting for the output speaker to start
          this->waiting_for_output_ = false;
          this->enter_stopping_state_();
        }
      }
      break;
    }
    case speaker::STATE_RUNNING:
      if (this->format_changed_.exchange(false)) {
        this->restart_after_format_ = true;
        this->waiting_for_output_ = false;
        this->enter_stopping_state_();
      } else if (this->resampling_ && !this->task_.is_created()) {
        // The task ended with its input (after finish()): the output finishes what it got on its own.
        this->resampling_ = false;
        this->finishing_.store(false);
        this->output_speaker_->finish();
        this->state_ = speaker::STATE_STOPPED;
      } else if (this->output_speaker_->is_stopped()) {
        this->enter_stopping_state_();
      }
      break;
    case speaker::STATE_STOPPING: {
      if ((this->output_speaker_->get_pause_state()) ||
          ((App.get_loop_component_start_time() - this->state_start_ms_) > STATE_TRANSITION_TIMEOUT_MS)) {
        // If output speaker is paused or stopping timeout exceeded, force stop
        this->output_speaker_->stop();
      }

      if (this->output_speaker_->is_stopped() && !this->task_.is_created()) {
        // Only transition to stopped state once the output speaker and resampler task are fully stopped
        this->waiting_for_output_ = false;
        this->state_ = speaker::STATE_STOPPED;
      }
      break;
    }
    case speaker::STATE_STOPPED:
      if (this->restart_after_format_) {
        this->restart_after_format_ = false;
        this->state_ = speaker::STATE_STARTING;
        break;
      }
      if (this->switch_pending_) {
        this->take_pending_output_();
        if (this->restart_after_switch_) {
          this->restart_after_switch_ = false;
          this->state_ = speaker::STATE_STARTING;
          break;
        }
      }
      event_group_bits = xEventGroupGetBits(this->event_group_);
      if (event_group_bits == 0) {
        // No pending events, disable loop to save CPU cycles
        this->disable_loop();
      }
      break;
  }
}

void ResamplerSpeaker::set_start_error_(esp_err_t err) {
  switch (err) {
    case ESP_ERR_NO_MEM:
      this->status_set_error(LOG_STR("Not enough memory"));
      break;
    default:
      this->status_set_error(LOG_STR("Failed to start"));
      break;
  }
}

size_t ResamplerSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_stopped()) {
    this->start();
  }
  // More after finish() (the next file right away): the task goes on.
  if (this->finishing_.exchange(false))
    xEventGroupClearBits(this->event_group_, ResamplingEventGroupBits::TASK_COMMAND_FINISH);

  size_t bytes_written = 0;
  if (this->state_ == speaker::STATE_RUNNING && this->audio_stream_info_ != this->started_info_) {
    if (!this->format_changed_.exchange(true))
      this->enable_loop_soon_any_context();
    vTaskDelay(ticks_to_wait);
  } else if (this->output_speaker_ == nullptr) {
    vTaskDelay(ticks_to_wait);
  } else if ((this->output_speaker_->is_running()) && (!this->requires_resampling_())) {
    bytes_written = this->output_speaker_->play(data, length, ticks_to_wait);
  } else {
    std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
    if (temp_ring_buffer != nullptr) {
      // Only write to the ring buffer if the reference is valid
      bytes_written = temp_ring_buffer->write_without_replacement(data, length, ticks_to_wait);
    } else {
      // Delay to avoid repeatedly hammering while waiting for the speaker to start
      vTaskDelay(ticks_to_wait);
    }
  }

  return bytes_written;
}

void ResamplerSpeaker::send_command_(uint32_t command_bit, bool wake_loop) {
  this->enable_loop_soon_any_context();
  uint32_t event_bits = xEventGroupGetBits(this->event_group_);
  if (!(event_bits & command_bit)) {
    xEventGroupSetBits(this->event_group_, command_bit);
    if (wake_loop) {
      App.wake_loop_threadsafe();
    }
  }
}

void ResamplerSpeaker::start() { this->send_command_(ResamplingEventGroupBits::COMMAND_START, true); }

void ResamplerSpeaker::select_output(size_t index) {
  if (index >= this->outputs_.size())
    return;
  this->wanted_key_ = this->outputs_[index].key;
  if (this->preference_key_ != 0 && this->outputs_.size() + this->groups_.size() > 1)
    this->pref_.save(&this->wanted_key_);
  this->switch_to_(this->outputs_[index].speaker);
}

void ResamplerSpeaker::switch_to_(speaker::Speaker *speaker) {
  this->pending_speaker_ = speaker;
  if (speaker == this->output_speaker_ && !this->switch_pending_)
    return;
  this->switch_pending_ = true;
  if (this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STARTING) {
    this->restart_after_switch_ = true;
    this->waiting_for_output_ = false;
    this->enter_stopping_state_();
  }
  this->enable_loop();
}

void ResamplerSpeaker::take_pending_output_() {
  this->switch_pending_ = false;
  const bool pause = this->get_pause_state();
  this->apply_output_(this->pending_speaker_);
  if (this->output_speaker_ == nullptr) {
    ESP_LOGI(TAG, "Output: none");
    return;
  }
  this->output_speaker_->set_volume(this->volume_);
  this->output_speaker_->set_mute_state(this->mute_state_);
  this->output_speaker_->set_pause_state(pause);
  ESP_LOGI(TAG, "Output: %s", this->output_name(this->active_));
  this->output_callback_.call(this->active_);
}

void ResamplerSpeaker::apply_output_(speaker::Speaker *speaker) {
  const size_t index = this->index_of_(speaker);
  if (index == this->outputs_.size()) {
    this->output_speaker_ = nullptr;
    return;
  }
  const Output &output = this->outputs_[index];
  this->active_ = index;
  this->output_speaker_ = output.speaker;
  this->output_channels_ = output.channels;
  this->target_sample_rate_ = output.sample_rate;
  this->passthrough_bits_per_sample_ = output.bits_per_sample == 0;
  if (output.bits_per_sample != 0)
    this->target_bits_per_sample_ = output.bits_per_sample;
}

audio::AudioStreamInfo ResamplerSpeaker::active_format() const {
  if (this->output_speaker_ == nullptr)
    return this->audio_stream_info_;
  const uint8_t bits =
      this->passthrough_bits_per_sample_ ? this->audio_stream_info_.get_bits_per_sample() : this->target_bits_per_sample_;
  const uint8_t channels = this->output_channels_ != 0 ? this->output_channels_ : this->audio_stream_info_.get_channels();
  return audio::AudioStreamInfo(bits, channels, this->target_sample_rate_);
}

esp_err_t ResamplerSpeaker::start_() {
  // In passthrough mode, the output keeps the input's bits per sample so only the sample rate is resampled.
  const uint8_t target_bits_per_sample = this->passthrough_bits_per_sample_
                                             ? this->audio_stream_info_.get_bits_per_sample()
                                             : this->target_bits_per_sample_;
  const uint8_t output_channels = this->output_channels_;
  this->target_stream_info_ = audio::AudioStreamInfo(
      target_bits_per_sample, output_channels != 0 ? output_channels : this->audio_stream_info_.get_channels(),
      this->target_sample_rate_);

  this->output_speaker_->set_audio_stream_info(this->target_stream_info_);
  this->output_speaker_->start();
  this->started_info_ = this->audio_stream_info_;

  this->resampling_ = this->requires_resampling_();
  if (this->resampling_) {
    // Start the resampler task to handle converting sample rates
    if (!this->task_.create(resample_task, "resampler", TASK_STACK_SIZE, (void *) this, RESAMPLER_TASK_PRIORITY,
                            this->task_stack_in_psram_, LOOP_TASK_CORE)) {
      return ESP_ERR_NO_MEM;
    }
  }

  return ESP_OK;
}

void ResamplerSpeaker::stop() { this->send_command_(ResamplingEventGroupBits::COMMAND_STOP); }

void ResamplerSpeaker::enter_stopping_state_() {
  this->finishing_.store(false);
  this->resampling_ = false;
  this->state_ = speaker::STATE_STOPPING;
  this->state_start_ms_ = App.get_loop_component_start_time();
  if (this->task_.is_created()) {
    xEventGroupSetBits(this->event_group_, ResamplingEventGroupBits::TASK_COMMAND_STOP);
  }
  if (this->output_speaker_ != nullptr)
    this->output_speaker_->stop();
}

void ResamplerSpeaker::finish() { this->send_command_(ResamplingEventGroupBits::COMMAND_FINISH); }

bool ResamplerSpeaker::has_buffered_data() const {
  bool has_ring_buffer_data = false;
  if (this->requires_resampling_()) {
    std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = this->ring_buffer_.lock();
    if (temp_ring_buffer != nullptr) {
      has_ring_buffer_data = (temp_ring_buffer->available() > 0);
    }
  }
  return has_ring_buffer_data || (this->output_speaker_ != nullptr && this->output_speaker_->has_buffered_data());
}

void ResamplerSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  if (this->output_speaker_ != nullptr)
    this->output_speaker_->set_mute_state(mute_state);
}

void ResamplerSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  if (this->output_speaker_ != nullptr)
    this->output_speaker_->set_volume(volume);
}

bool ResamplerSpeaker::requires_resampling_() const {
  if (this->audio_stream_info_.get_sample_rate() != this->target_sample_rate_) {
    return true;
  }
  const uint8_t output_channels = this->output_channels_;
  if (output_channels != 0 && output_channels != this->audio_stream_info_.get_channels()) {
    return true;
  }
  // In passthrough mode the bits per sample always matches the input, so it never forces resampling.
  return !this->passthrough_bits_per_sample_ &&
         (this->audio_stream_info_.get_bits_per_sample() != this->target_bits_per_sample_);
}

void ResamplerSpeaker::resample_task(void *params) {
  ResamplerSpeaker *this_resampler = static_cast<ResamplerSpeaker *>(params);

  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STARTING);

  {  // Ensure C++ objects fall out of scope for proper cleanup before stopping the task
    std::unique_ptr<audio::AudioResampler> resampler = make_unique<audio::AudioResampler>(
        this_resampler->audio_stream_info_.ms_to_bytes(TRANSFER_BUFFER_DURATION_MS),
        this_resampler->target_stream_info_.ms_to_bytes(TRANSFER_BUFFER_DURATION_MS));

    esp_err_t err = resampler->start(this_resampler->audio_stream_info_, this_resampler->target_stream_info_,
                                     this_resampler->taps_, this_resampler->filters_);

    if (err == ESP_OK) {
      std::shared_ptr<ring_buffer::RingBuffer> temp_ring_buffer = ring_buffer::RingBuffer::create(
          this_resampler->audio_stream_info_.ms_to_bytes(this_resampler->buffer_duration_ms_));

      if (temp_ring_buffer == nullptr) {
        err = ESP_ERR_NO_MEM;
      } else {
        this_resampler->ring_buffer_ = temp_ring_buffer;
        resampler->add_source(this_resampler->ring_buffer_);

        this_resampler->output_speaker_->set_audio_stream_info(this_resampler->target_stream_info_);
        resampler->add_sink(this_resampler->output_speaker_);
      }
    }

    if (err == ESP_OK) {
      xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_RUNNING);
    } else if (err == ESP_ERR_NO_MEM) {
      xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::ERR_ESP_NO_MEM);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
      xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::ERR_ESP_NOT_SUPPORTED);
    }

    while (err == ESP_OK) {
      uint32_t event_bits = xEventGroupGetBits(this_resampler->event_group_);

      if (event_bits & ResamplingEventGroupBits::TASK_COMMAND_STOP) {
        break;
      }

      // Stop gracefully if the decoder is done
      int32_t ms_differential = 0;
      audio::AudioResamplerState resampler_state =
          resampler->resample((event_bits & ResamplingEventGroupBits::TASK_COMMAND_FINISH) != 0, &ms_differential);

      if (resampler_state == audio::AudioResamplerState::FINISHED) {
        break;
      } else if (resampler_state == audio::AudioResamplerState::FAILED) {
        xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::ERR_ESP_FAIL);
        break;
      }
    }

    xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STOPPING);
  }

  xEventGroupSetBits(this_resampler->event_group_, ResamplingEventGroupBits::STATE_STOPPED);

  vTaskSuspend(nullptr);  // Suspend this task indefinitely until the loop method deletes it
}

}  // namespace esphome::resampler

#endif
