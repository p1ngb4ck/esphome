/**
 * @file buffered_file_reader.cpp
 * @brief Async storage-worker read -> PSRAM read-ahead ring -> read() / peek().
 */

#include "buffered_file_reader.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"


#include <algorithm>
#include <cstring>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "buffered_file_reader";

void BufferedFileReader::LookaheadRing::copy_out_(uint32_t pos, uint8_t *dst, size_t len) const {
  const size_t at = this->at(pos);
  const size_t first = std::min(len, this->size - at);
  memcpy(dst, this->data + at, first);
  if (first < len)
    memcpy(dst + first, this->data, len - first);
}

void BufferedFileReader::LookaheadRing::write(const uint8_t *src, size_t len) {
  const uint32_t pos = this->head.load(std::memory_order_relaxed);
  const size_t at = this->at(pos);
  const size_t first = std::min(len, this->size - at);
  memcpy(this->data + at, src, first);
  if (first < len)
    memcpy(this->data, src + first, len - first);
  this->head.store(this->advance(pos, len), std::memory_order_release);
}

size_t BufferedFileReader::LookaheadRing::read(uint8_t *dst, size_t len) {
  const uint32_t pos = this->tail.load(std::memory_order_relaxed);
  len = std::min(len, this->used(this->head.load(std::memory_order_acquire), pos));
  this->copy_out_(pos, dst, len);
  this->tail.store(this->advance(pos, len), std::memory_order_release);
  return len;
}

bool BufferedFileReader::LookaheadRing::peek(size_t offset, uint8_t *dst, size_t len) const {
  if (offset + len > this->available())
    return false;
  this->copy_out_(this->advance(this->tail.load(std::memory_order_relaxed), offset), dst, len);
  return true;
}

void BufferedFileReader::kick_fill_() {
  if (this->network_)
    return;  // filled by push()
  if (this->draining_.load(std::memory_order_acquire) || this->eof_.load(std::memory_order_acquire) ||
      this->fill_err_.load(std::memory_order_acquire))
    return;
  if (this->fill_in_flight_.exchange(true, std::memory_order_acq_rel))
    return;  // one already in flight

  // Only whole chunks: a request occupies the single in-flight slot for a full worker round trip,
  // so topping up a few bytes after every small read() halved the fill rate.
  size_t room = this->ring_.free();
  if (room < FILL_CHUNK) {
    this->fill_in_flight_.store(false, std::memory_order_release);  // read() re-kicks after draining
    return;
  }
  // Straight into the ring, up to its end: chunks start where the last one ended, so with whole
  // chunks (and a ring of whole chunks) one never wraps.
  const size_t at = this->ring_.at(this->ring_.head.load(std::memory_order_relaxed));
  const size_t want = std::min(FILL_CHUNK, this->ring_.size - at);
  this->fill_got_ = 0;
  this->fill_submit_us_ = micros();
  storage::StorageError submit = storage::global_storage_worker->read_chunk(
      this->handle_, this->ring_.data + at, want, &this->fill_got_,
      [this](storage::StorageError e) { this->on_fill_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGW(TAG, "read_chunk submit failed: %s", storage::error_to_string(submit));
    this->fill_in_flight_.store(false, std::memory_order_release);  // retried by the next kick
  }
}

void BufferedFileReader::on_fill_done_(storage::StorageError err) {
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "read_chunk failed: %s", storage::error_to_string(err));
    this->fill_err_.store(true, std::memory_order_release);
  } else if (this->fill_got_ == 0) {
    this->eof_.store(true, std::memory_order_release);
  } else {
    // Read into the ring's free part (read() only frees more), now handed to the reader.
    this->ring_.commit(this->fill_got_);
    const uint32_t dt = micros() - this->fill_submit_us_;
    this->stats_.chunks++;
    this->stats_.chunk_us += dt;
    if (dt > this->stats_.chunk_us_max)
      this->stats_.chunk_us_max = dt;
    this->stats_.bytes_filled += this->fill_got_;
  }
  this->fill_in_flight_.store(false, std::memory_order_release);
  if (this->waiting_task_ != nullptr)
    xTaskNotifyGive(this->waiting_task_);  // wake a quiesce_fill_() waiter, if any
  this->kick_fill_();  // chain the next read
}

void BufferedFileReader::quiesce_fill_() {
  // No new fill starts from here until draining_ is cleared again (open() / after a seek).
  this->draining_.store(true, std::memory_order_release);
  this->waiting_task_ = xTaskGetCurrentTaskHandle();
  uint32_t waited = 0;
  while (this->fill_in_flight_.load(std::memory_order_acquire)) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAIT_SLICE_MS)) == 0 && (waited += WAIT_SLICE_MS) >= WAIT_CAP_MS)
      break;
  }
}

bool BufferedFileReader::open(const char *path) {
  if (this->open_)
    this->close();
  if (storage::global_storage_registry == nullptr || storage::global_storage_worker == nullptr ||
      this->ring_.data == nullptr) {
    ESP_LOGE(TAG, "storage worker or read-ahead ring not available");
    return false;
  }
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(path, &rel);
  if (ps == nullptr) {
    ESP_LOGE(TAG, "'%s' does not resolve to a mounted storage", path);
    return false;
  }

  this->arm_wait_();
  if (storage::global_storage_worker->begin_read(ps, rel, &this->handle_,
                                                 [this](storage::StorageError e) { this->on_sync_done_(e); }) !=
          storage::StorageError::STORAGE_ERROR_OK ||
      this->wait_sync_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "begin_read('%s') failed", path);
    return false;
  }

  this->ring_.reset();
  this->open_ = true;
  this->current_position_ = 0;
  this->eof_.store(false);
  this->fill_err_.store(false);
  this->fill_in_flight_.store(false);
  this->draining_.store(false);
  this->stats_ = FillStats{};
  this->kick_fill_();  // start streaming
  return true;
}

void BufferedFileReader::close() {
  if (!this->open_)
    return;
  if (this->network_) {
    xSemaphoreTake(this->stream_lock_, portMAX_DELAY);
    this->open_ = false;
    this->network_ = false;
    this->ring_.reset();
    xSemaphoreGive(this->stream_lock_);
    this->streaming_ = false;
    this->current_position_ = 0;
    this->waiting_task_ = nullptr;
    return;
  }
  // The abort flag exists to abandon an in-flight read so playback stops promptly -- it must NOT
  // short-circuit end_read(), or the storage stream leaks (worker abandons it 30s later -> crash).
  // Clear it so this final close runs to completion synchronously while the task is still alive.
  this->abort_flag_ = nullptr;
  this->quiesce_fill_();
  this->arm_wait_();
  storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_sync_done_(e); });
  this->wait_sync_();

  this->ring_.reset();
  this->open_ = false;
  this->streaming_ = false;
  this->current_position_ = 0;
  this->waiting_task_ = nullptr;  // no completion is due any more
}

bool BufferedFileReader::open_stream() {
  if (this->open_)
    this->close();
  if (this->stream_lock_ == nullptr && (this->stream_lock_ = xSemaphoreCreateMutex()) == nullptr) {
    ESP_LOGE(TAG, "could not create the stream lock");
    return false;
  }
  xSemaphoreTake(this->stream_lock_, portMAX_DELAY);
  if (this->ring_.data == nullptr) {
    xSemaphoreGive(this->stream_lock_);
    ESP_LOGE(TAG, "no read-ahead ring");
    return false;
  }
  this->ring_.reset();
  this->network_ = true;
  this->current_position_ = 0;
  this->eof_.store(false);
  this->fill_err_.store(false);
  this->fill_in_flight_.store(false);
  this->draining_.store(false);
  this->stats_ = FillStats{};
  this->open_ = true;
  xSemaphoreGive(this->stream_lock_);
  return true;
}

size_t BufferedFileReader::stream_room() {
  if (this->stream_lock_ == nullptr)
    return 0;
  xSemaphoreTake(this->stream_lock_, portMAX_DELAY);
  const size_t room = this->open_ && this->network_ ? this->ring_.free() : 0;
  xSemaphoreGive(this->stream_lock_);
  return room;
}

void BufferedFileReader::push(const uint8_t *data, size_t len) {
  if (this->stream_lock_ == nullptr)
    return;
  xSemaphoreTake(this->stream_lock_, portMAX_DELAY);
  if (this->open_ && this->network_) {
    this->ring_.write(data, std::min(len, this->ring_.free()));
    this->stats_.chunks++;
    this->stats_.bytes_filled += len;
  }
  xSemaphoreGive(this->stream_lock_);
}

void BufferedFileReader::end_stream() {
  if (this->network_)
    this->eof_.store(true, std::memory_order_release);
}

bool BufferedFileReader::prefill_cache() {
  if (!this->open_)
    return false;
  // LOAD context: block (bounded) until the read-ahead ring is full. The kick_fill_ ->
  // on_fill_done_ chain self-continues; on_fill_done_ notifies waiting_task_ after each chunk.
  this->waiting_task_ = xTaskGetCurrentTaskHandle();
  this->kick_fill_();
  uint32_t waited = 0;
  while (this->ring_.free() > FILL_CHUNK) {
    if (this->fill_err_.load(std::memory_order_acquire))
      return false;
    if (this->eof_.load(std::memory_order_acquire))
      break;  // whole file fit in the ring
    if (this->abort_flag_ != nullptr && *this->abort_flag_)
      return false;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAIT_SLICE_MS));
    if ((waited += WAIT_SLICE_MS) >= WAIT_CAP_MS)
      break;
    this->kick_fill_();
  }
  return true;
}

// From the reader task (or a LOAD-time probe): what the ring has, and for the rest a wait for the
// fill chain (woken by each completion) until it is there, the file ends, a read fails, playback
// stops or WAIT_CAP_MS pass. A slow storage then delays frames instead of ending the video.
int BufferedFileReader::read(uint8_t *buffer, size_t size) {
  if (!this->open_)
    return -1;
  if (this->streaming_) {
    const size_t avail = this->ring_.available();
    if (!this->stats_.min_set || avail < this->stats_.min_avail) {
      this->stats_.min_avail = avail;
      this->stats_.min_set = true;
    }
  }
  size_t copied = 0;
  uint32_t waited = 0;
  bool short_counted = false;
  while (copied < size) {
    const size_t n = this->ring_.read(buffer + copied, size - copied);
    if (n > 0) {
      copied += n;
      this->current_position_ += n;
      waited = 0;
      this->kick_fill_();  // made room -- keep the stream fed
      continue;
    }
    if (this->fill_err_.load(std::memory_order_acquire))
      return copied > 0 ? static_cast<int>(copied) : -1;
    if (this->eof_.load(std::memory_order_acquire) && this->ring_.available() == 0)
      break;  // stream ended and ring drained
    if (this->abort_flag_ != nullptr && *this->abort_flag_)
      return copied > 0 ? static_cast<int>(copied) : -1;
    if (this->streaming_ && !short_counted) {
      this->stats_.underruns++;
      short_counted = true;
    }
    this->kick_fill_();
    this->waiting_task_ = xTaskGetCurrentTaskHandle();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAIT_SLICE_MS));
    if ((waited += WAIT_SLICE_MS) >= WAIT_CAP_MS)
      return copied > 0 ? static_cast<int>(copied) : -1;
  }
  return static_cast<int>(copied);
}

bool BufferedFileReader::peek(size_t offset, uint8_t *buffer, size_t size) const {
  return this->open_ && this->ring_.peek(offset, buffer, size);
}

bool BufferedFileReader::seek(uint64_t position) {
  if (!this->open_)
    return false;
  if (this->network_) {
    // A stream only goes forward: what lies before the read position is gone.
    if (position < this->current_position_)
      return false;
    uint8_t scratch[64];
    while (this->current_position_ < position) {
      const size_t want = static_cast<size_t>(std::min<uint64_t>(sizeof(scratch), position - this->current_position_));
      if (this->read(scratch, want) <= 0)
        return false;
    }
    return true;
  }
  this->quiesce_fill_();
  this->ring_.reset();
  this->eof_.store(false);
  this->fill_err_.store(false);

  this->arm_wait_();
  if (storage::global_storage_worker->seek(this->handle_, static_cast<int64_t>(position),
                                           storage::SeekMode::SEEK_MODE_SET,
                                           [this](storage::StorageError e) { this->on_sync_done_(e); }) !=
          storage::StorageError::STORAGE_ERROR_OK ||
      this->wait_sync_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "seek failed");
    return false;
  }
  this->current_position_ = position;
  this->draining_.store(false);  // allow fills again
  this->kick_fill_();
  return true;
}

bool BufferedFileReader::get_size(uint64_t *size) {
  if (!this->open_ || this->network_)
    return false;
  uint64_t saved = this->current_position_;
  this->quiesce_fill_();

  this->arm_wait_();
  if (storage::global_storage_worker->seek(this->handle_, 0, storage::SeekMode::SEEK_MODE_END,
                                           [this](storage::StorageError e) { this->on_sync_done_(e); }) !=
          storage::StorageError::STORAGE_ERROR_OK ||
      this->wait_sync_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: seek(END) failed");
    return false;
  }
  uint64_t end_pos = 0;
  this->arm_wait_();
  if (storage::global_storage_worker->tell(this->handle_, &end_pos,
                                           [this](storage::StorageError e) { this->on_sync_done_(e); }) !=
          storage::StorageError::STORAGE_ERROR_OK ||
      this->wait_sync_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: tell failed");
    return false;
  }
  if (!this->seek(saved))  // also re-primes the fill chain from `saved`
    return false;
  *size = end_pos;
  return true;
}

}  // namespace simple_video_player
}  // namespace esphome
