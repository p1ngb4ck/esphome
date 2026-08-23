#include "usb_audio_speaker.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "../usb_audio.h"

#include <algorithm>
#include <cinttypes>

#include "esphome/core/log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace usb_audio {

static const char *const TAG_SPK = "usb_audio.spk";

namespace {
// Upper bound on the time one play() call may spend pushing data into the USB ring buffer.
constexpr uint32_t MAX_WORK_TIME_MS = 20;
// Share of the work budget a single blocking write may consume. The isochronous sink drains
// at exactly real time, so a chunk worth more playback time than this can only time out.
constexpr uint32_t WRITE_BUDGET_DIVISOR = 2;
// Window over which the accepted byte rate is measured before it is judged.
constexpr uint32_t RATE_WINDOW_MS = 1000;
// Fraction of the nominal sample rate the sink must keep up with, in percent. A short write
// is normal backpressure: the sink drains at exactly real time and the caller keeps the
// remainder. Only a sustained shortfall below this means playback is actually breaking up.
constexpr uint32_t RATE_WARN_PERCENT = 80;
// How long finish() waits for the queue to drain before stopping anyway.
constexpr uint32_t FINISH_TIMEOUT_MS = 1000;
}  // namespace

void USBAudioSpeaker::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(this->bits_per_sample_, this->channels_, this->sample_rate_);
}

void USBAudioSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG_SPK, "USB Speaker:");
  ESP_LOGCONFIG(TAG_SPK, "  Sample rate: %lu Hz", this->sample_rate_);
  ESP_LOGCONFIG(TAG_SPK, "  Bits per sample: %u", this->bits_per_sample_);
  ESP_LOGCONFIG(TAG_SPK, "  Channels: %u", this->channels_);
  if (this->channels_ > 2) {
    ESP_LOGW(TAG_SPK, "USB speaker only supports mono or stereo playback; additional channels will be ignored");
  }
}

void USBAudioSpeaker::loop() {
  if (this->finish_requested_) {
    if (!this->has_buffered_data() || millis() > this->finish_deadline_ms_) {
      this->stop();
      this->finish_requested_ = false;
    }
  }
}

void USBAudioSpeaker::start() {
  if (this->state_ == speaker::STATE_RUNNING) {
    return;
  }
  if (!this->ensure_started_()) {
    ESP_LOGE(TAG_SPK, "USB host not started");
    this->status_set_warning();
    return;
  }
  this->parent_->resume_speaker();
  this->state_ = speaker::STATE_RUNNING;
  this->status_clear_warning();
}

void USBAudioSpeaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED) {
    return;
  }
  this->parent_->suspend_speaker();
  this->state_ = speaker::STATE_STOPPED;
  this->finish_requested_ = false;
  this->pause_state_ = false;
  this->rate_window_start_ms_ = 0;
}

void USBAudioSpeaker::set_pause_state(bool pause_state) {
  if (this->pause_state_ == pause_state || this->parent_ == nullptr) {
    return;
  }
  this->pause_state_ = pause_state;
  // Suspending feeds the endpoint silence rather than closing it: the device keeps its
  // sample clock and the alt-setting stays selected, so resuming is immediate.
  if (pause_state) {
    this->parent_->suspend_speaker();
  } else {
    this->parent_->resume_speaker();
    this->rate_window_start_ms_ = 0;
  }
}

void USBAudioSpeaker::finish() {
  if (!this->is_running()) {
    this->stop();
    return;
  }
  this->finish_requested_ = true;
  this->finish_deadline_ms_ = millis() + FINISH_TIMEOUT_MS;
}


size_t USBAudioSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  // The caller's block time is the budget; MAX_WORK_TIME_MS only caps how long one call may
  // hold the audio task.
  uint32_t time_budget_ms = MAX_WORK_TIME_MS;
  if (ticks_to_wait != portMAX_DELAY) {
    const uint32_t ticks_ms = static_cast<uint32_t>(ticks_to_wait) * portTICK_PERIOD_MS;
    if (ticks_ms > 0) {
      time_budget_ms = std::min(time_budget_ms, ticks_ms);
    }
  }
  return this->play_internal_(data, length, time_budget_ms);
}

size_t USBAudioSpeaker::play(const uint8_t *data, size_t length) {
  return this->play_internal_(data, length, MAX_WORK_TIME_MS);
}

size_t USBAudioSpeaker::play_internal_(const uint8_t *data, size_t length, uint32_t time_budget_ms) {
  if (!this->is_running()) {
    this->start();
  }
  if (!this->is_running() || data == nullptr || length == 0) {
    return 0;
  }

  const uint32_t work_budget_ms =
      (time_budget_ms > 0) ? std::min<uint32_t>(time_budget_ms, MAX_WORK_TIME_MS) : MAX_WORK_TIME_MS;
  const uint32_t start_ms = millis();

  const size_t bytes_per_frame =
      std::max<size_t>(1, static_cast<size_t>(this->channels_) * std::max<size_t>(1, this->bits_per_sample_ / 8));

  // Write granularity is the isochronous packet the sink consumes per service interval, so a
  // write never leaves a partial packet behind. Fall back to a single audio frame while the
  // stream is not open yet and the packet size is therefore unknown.
  size_t packet_bytes = this->parent_->get_speaker_packet_bytes();
  if (packet_bytes < bytes_per_frame) {
    packet_bytes = bytes_per_frame;
  }

  // The sink drains at real time and never faster: sample_rate frames per second. Asking a
  // blocking write for more playback time than it may block for can only ever time out, so
  // cap one chunk at what drains within a fraction of the budget. Deriving this from the
  // stream format keeps it correct for any rate the endpoint was opened with.
  const size_t bytes_per_ms = std::max<size_t>(1, (static_cast<size_t>(this->sample_rate_) * bytes_per_frame) / 1000);
  size_t max_chunk = bytes_per_ms * std::max<uint32_t>(1, work_budget_ms / WRITE_BUDGET_DIVISOR);
  max_chunk = (max_chunk / packet_bytes) * packet_bytes;
  if (max_chunk == 0) {
    max_chunk = packet_bytes;
  }

  size_t total_written = 0;
  size_t remaining = length;
  const uint8_t *current = data;

  while (remaining >= bytes_per_frame) {
    const uint32_t elapsed_ms = millis() - start_ms;
    if (elapsed_ms >= work_budget_ms) {
      break;
    }

    // Whole packets while there is enough left for one, whole frames for the tail.
    size_t chunk = std::min(remaining, max_chunk);
    size_t aligned = (chunk / packet_bytes) * packet_bytes;
    if (aligned == 0) {
      aligned = (chunk / bytes_per_frame) * bytes_per_frame;
    }
    chunk = aligned;
    if (chunk == 0) {
      break;
    }

    uint32_t call_timeout_ms = work_budget_ms - elapsed_ms;
    if (call_timeout_ms == 0) {
      call_timeout_ms = 1;
    }

    const esp_err_t err = this->parent_->write_speaker(current, chunk, call_timeout_ms);
    if (err != ESP_OK) {
      // A short write is a normal outcome, not an error: the sink drains at exactly real
      // time and the caller keeps the remainder. Retrying with a smaller chunk cannot help,
      // because the drain rate is fixed by the sample clock. Anything other than a timeout
      // is a real fault and is reported immediately.
      if (err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG_SPK, "Speaker write failed: %s", esp_err_to_name(err));
        this->rate_window_start_ms_ = 0;
      }
      break;
    }

    total_written += chunk;
    current += chunk;
    remaining -= chunk;

    const size_t frames_written = chunk / bytes_per_frame;
    if (frames_written > 0) {
      const int64_t timestamp_us = esp_timer_get_time();
      this->audio_output_callback_(static_cast<uint32_t>(frames_written), timestamp_us);
    }
  }

  this->check_throughput_(total_written, bytes_per_ms);
  return total_written;
}

// Judge the sink by throughput, not by whether a single write was short. Over a one second
// window the accepted bytes have to track the sample clock; a sustained shortfall is the one
// symptom that actually means the audio is breaking up (a stream that stopped draining, or a
// device that fell behind), and it is worth one warning per window.
void USBAudioSpeaker::check_throughput_(size_t accepted, size_t bytes_per_ms) {
  const uint32_t now = millis();
  if (this->rate_window_start_ms_ == 0) {
    this->rate_window_start_ms_ = now;
    this->rate_window_bytes_ = accepted;
    return;
  }
  this->rate_window_bytes_ += accepted;
  const uint32_t elapsed_ms = now - this->rate_window_start_ms_;
  if (elapsed_ms < RATE_WINDOW_MS)
    return;

  const size_t expected = bytes_per_ms * elapsed_ms;
  const size_t floor_bytes = expected / 100 * RATE_WARN_PERCENT;
  if (this->rate_window_bytes_ < floor_bytes) {
    ESP_LOGW(TAG_SPK, "Speaker underrunning: accepted %u of %u bytes over %" PRIu32 " ms",
             (unsigned) this->rate_window_bytes_, (unsigned) expected, elapsed_ms);
  }
  this->rate_window_start_ms_ = now;
  this->rate_window_bytes_ = 0;
}

bool USBAudioSpeaker::has_buffered_data() const {
  if (!this->is_running() || this->parent_ == nullptr) {
    return false;
  }
  // Ask the queue rather than inferring it from the time of the last write, which reports
  // "buffered" for a stream that has long since drained and "empty" for a paused one.
  return this->parent_->get_speaker_queued_bytes() > 0;
}

void USBAudioSpeaker::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);
  if (this->parent_ != nullptr) {
    this->parent_->set_speaker_volume_level(volume);
  }
}

void USBAudioSpeaker::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);
  if (this->parent_ != nullptr) {
    this->parent_->set_speaker_mute_state(mute_state);
  }
}

bool USBAudioSpeaker::ensure_started_() { return this->parent_->ensure_started_speaker(); }

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
