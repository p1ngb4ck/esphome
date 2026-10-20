#include "usb_audio_microphone.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "../usb_audio.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <string>

extern "C" {
#include "esp_err.h"
}

namespace esphome {
namespace usb_audio {

static const char *const TAG_MIC = "usb_audio.mic";

void USBAudioMicrophone::setup() {
  uint32_t buffer_size = this->parent_->get_microphone_buffer_size();
  if (buffer_size == 0) {
    buffer_size = USB_AUDIO_DEFAULT_BUFFER_SIZE;
  }
  this->read_buffer_.reserve(buffer_size);

  this->audio_stream_info_ = audio::AudioStreamInfo(this->bits_per_sample_, this->channels_, this->sample_rate_);

  // The configuration validates the indices, so every one is below channels_.
  for (uint8_t channel : this->enabled_channels_) {
    this->enabled_mask_ |= static_cast<uint8_t>(1u << channel);
  }
  if (this->enabled_mask_ != 0) {
    this->primary_channel_ = static_cast<uint8_t>(__builtin_ctz(this->enabled_mask_));
  }

  if (!this->mic_task_.create(&USBAudioMicrophone::mic_task, "usb_mic", MIC_TASK_STACK_SIZE, this, MIC_TASK_PRIORITY,
                              true)) {
    this->mark_failed(LOG_STR("Failed to create microphone task"));
    return;
  }
  // start() and stop() wake the loop.
  this->disable_loop();
}

void USBAudioMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG_MIC, "USB Microphone:");
  ESP_LOGCONFIG(TAG_MIC, "  Sample rate: %" PRIu32 " Hz", this->sample_rate_);
  ESP_LOGCONFIG(TAG_MIC, "  Bits per sample: %u", this->bits_per_sample_);
  ESP_LOGCONFIG(TAG_MIC, "  Source channels: %u", this->channels_);
  if (!this->enabled_channels_.empty()) {
    std::string channel_list;
    for (size_t i = 0; i < this->enabled_channels_.size(); ++i) {
      if (i != 0) {
        channel_list.append(", ");
      }
      channel_list.append(std::to_string(this->enabled_channels_[i]));
    }
    ESP_LOGCONFIG(TAG_MIC, "  Enabled channel indices: [%s]", channel_list.c_str());
  }
  ESP_LOGCONFIG(TAG_MIC, "  Buffer size: %u", static_cast<unsigned int>(this->read_buffer_.capacity()));
}

void USBAudioMicrophone::request_(uint8_t set, uint8_t clear) {
  this->commands_.fetch_and(static_cast<uint8_t>(~clear));
  this->commands_.fetch_or(set);
  this->enable_loop_soon_any_context();
}

// Every MicrophoneSource forwards its own start() and stop(), so capture runs while any of
// them is listening, like i2s_audio's listener count.
void USBAudioMicrophone::start() {
  if (this->is_failed()) {
    return;
  }
  uint8_t listeners = this->listeners_.load();
  do {
    if (listeners >= MAX_LISTENERS) {
      return;
    }
  } while (!this->listeners_.compare_exchange_weak(listeners, listeners + 1));
  if (listeners == 0) {
    this->request_(COMMAND_START, COMMAND_STOP);
  }
}

void USBAudioMicrophone::stop() {
  uint8_t listeners = this->listeners_.load();
  do {
    if (listeners == 0) {
      return;
    }
  } while (!this->listeners_.compare_exchange_weak(listeners, listeners - 1));
  if (listeners == 1) {
    this->request_(COMMAND_STOP, COMMAND_START);
  }
}

void USBAudioMicrophone::loop() {
  const uint8_t commands = this->commands_.exchange(0);
  if (commands & COMMAND_STOP) {
    this->cancel_timeout(RETRY_START_ID);
    this->retry_delay_ms_ = RETRY_MIN_MS;
    if (this->state_ == microphone::STATE_STARTING || this->state_ == microphone::STATE_RUNNING) {
      this->parent_->suspend_microphone();
      // The task notices within one read timeout; the stop finishes below once it is idle.
      this->running_ = false;
      this->state_ = microphone::STATE_STOPPING;
    }
  }
  if (this->state_ == microphone::STATE_STOPPING && this->task_idle_) {
    this->state_ = microphone::STATE_STOPPED;
  }
  if ((commands & COMMAND_START) && this->listeners_ > 0) {
    if (this->state_ == microphone::STATE_STOPPED) {
      this->state_ = microphone::STATE_STARTING;
      this->open_stream_();
    } else if (this->state_ == microphone::STATE_STOPPING) {
      // Started again once the task is idle.
      this->commands_.fetch_or(COMMAND_START);
    }
  }
  if (this->state_ != microphone::STATE_STOPPING) {
    this->disable_loop();
  }
}

// Answered by on_stream_event() when the open is still under way; a device that is not there
// yet opens the stream itself once it connects.
void USBAudioMicrophone::open_stream_() {
  if (!this->parent_->resume_microphone()) {
    if (this->parent_->device_connected()) {
      this->on_stream_event(MicStreamEvent::FAILED);
    }
    return;
  }
  if (this->parent_->microphone_stream_open()) {
    this->set_running_();
  }
}

void USBAudioMicrophone::set_running_() {
  this->state_ = microphone::STATE_RUNNING;
  this->retry_delay_ms_ = RETRY_MIN_MS;
  this->status_clear_warning();
  // After a lost stream the task is still reading and picks the stream up by itself.
  if (!this->running_) {
    this->task_idle_ = false;
    this->running_ = true;
    xTaskNotifyGive(this->mic_task_.get_handle());
  }
}

void USBAudioMicrophone::on_stream_event(MicStreamEvent event) {
  switch (event) {
    case MicStreamEvent::OPEN:
      if (this->state_ == microphone::STATE_STARTING) {
        this->set_running_();
      }
      break;
    case MicStreamEvent::LOST:
      if (this->state_ == microphone::STATE_RUNNING) {
        ESP_LOGW(TAG_MIC, "Stream lost, waiting for it to reopen");
        this->state_ = microphone::STATE_STARTING;
        // A stream that died with the device still there is not reopened by a reconnect.
        if (this->parent_->device_connected()) {
          this->on_stream_event(MicStreamEvent::FAILED);
        }
      }
      break;
    case MicStreamEvent::FAILED:
      if (this->state_ == microphone::STATE_STARTING && this->listeners_ > 0) {
        this->status_set_warning();
        this->set_timeout(RETRY_START_ID, this->retry_delay_ms_, [this]() {
          if (this->listeners_ > 0 && this->state_ == microphone::STATE_STARTING) {
            this->open_stream_();
          }
        });
        this->retry_delay_ms_ = std::min(this->retry_delay_ms_ * 2, RETRY_MAX_MS);
      }
      break;
  }
}

void USBAudioMicrophone::mic_task(void *param) { static_cast<USBAudioMicrophone *>(param)->mic_task_loop_(); }

void USBAudioMicrophone::mic_task_loop_() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (this->running_) {
      size_t bytes_read = 0;
      this->read_buffer_.resize(this->read_buffer_.capacity());
      esp_err_t err = this->parent_->read_microphone(this->read_buffer_.data(), this->read_buffer_.size(),
                                                     &bytes_read, READ_TIMEOUT_MS);
      if (!this->running_) {
        break;
      }
      if (err == ESP_OK && bytes_read > 0) {
        this->read_buffer_.resize(bytes_read);
        this->apply_channel_selection_(this->read_buffer_.data(), bytes_read);
        this->data_callbacks_.call(this->read_buffer_);
      } else if (err == ESP_ERR_TIMEOUT) {
        // read_microphone() already waited READ_TIMEOUT_MS for data.
      } else if (err == ESP_ERR_INVALID_STATE) {
        // The stream is opening, or reopening after it was lost.
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
      } else if (err != ESP_OK) {
        ESP_LOGW(TAG_MIC, "Read error: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
      }
    }
    this->task_idle_ = true;
  }
}

void USBAudioMicrophone::apply_channel_selection_(uint8_t *data, size_t length) {
  if (this->enabled_mask_ == 0) {
    return;
  }
  const size_t bytes_per_sample = static_cast<size_t>(this->bits_per_sample_) / 8U;
  const size_t channel_stride = static_cast<size_t>(this->channels_) * bytes_per_sample;
  if (bytes_per_sample == 0 || channel_stride == 0 || (length % channel_stride) != 0) {
    if (!this->logged_invalid_frame_) {
      ESP_LOGW(TAG_MIC, "Unable to apply channel selection: unexpected frame size (%u bytes)",
               static_cast<unsigned int>(length));
      this->logged_invalid_frame_ = true;
    }
    return;
  }
  const size_t sample_count = length / channel_stride;
  for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
    uint8_t *sample_base = data + sample_index * channel_stride;
    const uint8_t *primary_source = sample_base + this->primary_channel_ * bytes_per_sample;
    for (size_t channel_index = 0; channel_index < this->channels_; ++channel_index) {
      if (!(this->enabled_mask_ & (1u << channel_index))) {
        std::memcpy(sample_base + channel_index * bytes_per_sample, primary_source, bytes_per_sample);
      }
    }
  }
}

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
