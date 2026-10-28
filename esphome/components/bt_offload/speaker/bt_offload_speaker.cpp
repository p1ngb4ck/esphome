#include "bt_offload_speaker.h"

#ifdef USE_BT_OFFLOAD_SPEAKER

#include "esphome/components/usb_bluetooth/c6_offload.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>

namespace esphome::bt_offload {

static const char *const TAG = "bt_offload.speaker";

static constexpr uint32_t MSG_AUDIO_PCM = 0x42545032u;

void BTOffloadSpeaker::setup() {
  this->space_ = xSemaphoreCreateBinaryStatic(&this->space_control_);
  this->parent_->set_speaker(this);
  this->disable_loop();
}

void BTOffloadSpeaker::dump_config() { ESP_LOGCONFIG(TAG, "Bluetooth offload speaker: 16 bit stereo, 44100 Hz"); }

size_t BTOffloadSpeaker::write_(const uint8_t *data, size_t length) {
  LockGuard guard(this->lock_);
  if (this->ring_ == nullptr)
    return 0;
  return this->ring_->write_without_replacement(data, length, 0, true);
}

size_t BTOffloadSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->state_ != speaker::STATE_RUNNING)
    this->start();
  if (this->state_ != speaker::STATE_RUNNING)
    return 0;
  if (!this->connected_.load())
    return length;
  size_t written = this->write_(data, length);
  if (written == 0 && ticks_to_wait > 0 && xSemaphoreTake(this->space_, ticks_to_wait) == pdTRUE)
    written = this->write_(data, length);
  if (written > 0)
    this->enable_loop_soon_any_context();
  return written;
}

void BTOffloadSpeaker::start() {
  LockGuard guard(this->lock_);
  if (this->ring_ == nullptr) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < RING_BYTES) {
      ESP_LOGE(TAG, "No PSRAM block of %u bytes for the PCM ring", static_cast<unsigned>(RING_BYTES));
      return;
    }
    this->ring_ = ring_buffer::RingBuffer::create(RING_BYTES);
    if (this->ring_ == nullptr) {
      ESP_LOGE(TAG, "PCM ring of %u bytes not created", static_cast<unsigned>(RING_BYTES));
      return;
    }
  }
  this->state_ = speaker::STATE_RUNNING;
}

void BTOffloadSpeaker::stop() {
  this->state_ = speaker::STATE_STOPPED;
  this->enable_loop_soon_any_context();
}

void BTOffloadSpeaker::finish() {
  if (this->state_ == speaker::STATE_RUNNING) {
    this->state_ = speaker::STATE_STOPPING;
    this->enable_loop_soon_any_context();
  }
}

bool BTOffloadSpeaker::has_buffered_data() const {
  LockGuard guard(this->lock_);
  return this->ring_ != nullptr && this->ring_->available() > 0;
}

void BTOffloadSpeaker::release_() {
  LockGuard guard(this->lock_);
  if (this->state_ == speaker::STATE_STOPPED)
    this->ring_.reset();
}

size_t BTOffloadSpeaker::fill_(uint8_t *dst, size_t len, void *ctx) {
  auto *self = static_cast<BTOffloadSpeaker *>(ctx);
  return self->ring_->read(dst, len, 0);
}

void BTOffloadSpeaker::loop() {
  if (this->state_ == speaker::STATE_STOPPED) {
    this->release_();
    this->disable_loop();
    return;
  }
  bool sent = false;
  while (this->connected_.load() && this->credit_ > 0) {
    LockGuard guard(this->lock_);
    if (this->ring_ == nullptr)
      break;
    const size_t available = this->ring_->available();
    const bool last = this->state_ == speaker::STATE_STOPPING;
    size_t len = std::min<size_t>(std::min<size_t>(available, CHUNK), this->credit_);
    len -= len % BYTES_PER_FRAME;
    if (len == 0 || (len < CHUNK && !last))
      break;
    if (!usb_bluetooth::c6_offload::post_filled(MSG_AUDIO_PCM, len, &BTOffloadSpeaker::fill_, this))
      break;
    this->credit_ -= len;
    sent = true;
  }
  if (sent)
    xSemaphoreGive(this->space_);
  if (this->state_ == speaker::STATE_STOPPING && (!this->connected_.load() || !this->has_buffered_data())) {
    this->state_ = speaker::STATE_STOPPED;
    this->release_();
  }
  this->disable_loop();
}

void BTOffloadSpeaker::on_audio_state(bool connected) {
  this->connected_.store(connected);
  this->credit_ = connected ? C6_RING : 0;
  {
    LockGuard guard(this->lock_);
    if (this->ring_ != nullptr)
      this->ring_->reset();
  }
  if (connected)
    this->apply_volume_();
  this->enable_loop();
}

void BTOffloadSpeaker::on_credit(uint32_t bytes) {
  this->credit_ = std::min<uint32_t>(this->credit_ + bytes, C6_RING);
  this->audio_output_callback_.call(bytes / BYTES_PER_FRAME, esp_timer_get_time());
  this->enable_loop();
}

void BTOffloadSpeaker::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);
  this->apply_volume_();
}

void BTOffloadSpeaker::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);
  this->apply_volume_();
}

void BTOffloadSpeaker::apply_volume_() {
  if (!this->connected_.load())
    return;
  const float volume = this->is_silent_() ? 0.0f : std::min(this->volume_, 1.0f);
  this->parent_->set_audio_volume(static_cast<uint8_t>(std::lround(volume * 127.0f)));
}

}

#endif
