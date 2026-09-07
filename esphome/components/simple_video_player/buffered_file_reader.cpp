/**
 * @file buffered_file_reader.cpp
 * @brief Async storage-worker read -> transfer arena -> ring_buffer::RingBuffer -> read().
 */

#include "buffered_file_reader.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "buffered_file_reader";

void BufferedFileReader::kick_fill_() {
  if (this->draining_.load(std::memory_order_acquire) || this->eof_.load(std::memory_order_acquire) ||
      this->fill_err_.load(std::memory_order_acquire))
    return;
  if (this->fill_in_flight_.exchange(true, std::memory_order_acq_rel))
    return;  // one already in flight

  size_t room = this->ring_->free();
  if (room == 0) {
    this->fill_in_flight_.store(false, std::memory_order_release);  // ring full -- read() re-kicks after draining
    return;
  }
  size_t want = std::min(FILL_CHUNK, room);
  this->fill_got_ = 0;
  storage::StorageError submit = storage::global_storage_worker->read_chunk(
      this->handle_, this->arena_, want, &this->fill_got_,
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
    // want was <= ring free space and read() only frees more, so this always fits whole.
    this->ring_->write_without_replacement(this->arena_, this->fill_got_, 0, true);
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
      storage::global_transfer_buffer == nullptr) {
    ESP_LOGE(TAG, "storage worker / transfer buffer not available");
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

  this->arena_ = storage::global_transfer_buffer->try_acquire(FILL_CHUNK);
  if (this->arena_ == nullptr) {
    ESP_LOGE(TAG, "storage transfer buffer unavailable");
    this->arm_wait_();
    storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_sync_done_(e); });
    this->wait_sync_();
    return false;
  }
  this->ring_ = ring_buffer::RingBuffer::create(RING_BYTES);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "could not create %zu-byte ring buffer", RING_BYTES);
    storage::global_transfer_buffer->release();
    this->arena_ = nullptr;
    this->arm_wait_();
    storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_sync_done_(e); });
    this->wait_sync_();
    return false;
  }

  this->open_ = true;
  this->current_position_ = 0;
  this->eof_.store(false);
  this->fill_err_.store(false);
  this->fill_in_flight_.store(false);
  this->draining_.store(false);
  this->kick_fill_();  // start streaming
  return true;
}

void BufferedFileReader::close() {
  if (!this->open_)
    return;
  // The abort flag exists to abandon an in-flight read so playback stops promptly -- it must NOT
  // short-circuit end_read(), or the storage stream leaks (worker abandons it 30s later -> crash).
  // Clear it so this final close runs to completion synchronously while the task is still alive.
  this->abort_flag_ = nullptr;
  this->quiesce_fill_();
  this->arm_wait_();
  storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_sync_done_(e); });
  this->wait_sync_();

  if (storage::global_transfer_buffer != nullptr)
    storage::global_transfer_buffer->release();
  this->arena_ = nullptr;
  this->ring_.reset();
  this->open_ = false;
  this->streaming_ = false;  // next open() starts back in blocking (LOAD) mode
  this->current_position_ = 0;
}

bool BufferedFileReader::prefill_cache() {
  if (!this->open_)
    return false;
  // LOAD context: block (bounded) until the read-ahead ring is full. The kick_fill_ ->
  // on_fill_done_ chain self-continues; on_fill_done_ notifies waiting_task_ after each chunk.
  this->waiting_task_ = xTaskGetCurrentTaskHandle();
  this->kick_fill_();
  uint32_t waited = 0;
  while (this->ring_->free() > FILL_CHUNK) {
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

int BufferedFileReader::read(uint8_t *buffer, size_t size) {
  if (!this->open_)
    return -1;

  if (this->streaming_) {
    // PLAY path: single non-blocking drain. The storage worker streams chunks in ahead via the
    // async read_chunk() completion chain (on_fill_done_ -> kick_fill_), so in normal playback the
    // ring is already primed and this returns the full `size`. Never parks, never spins.
    size_t n = this->ring_->read(buffer, size, 0);
    this->current_position_ += n;
    this->kick_fill_();  // keep the async stream fed
    if (n == 0) {
      if (this->fill_err_.load(std::memory_order_acquire))
        return -1;
      if (this->eof_.load(std::memory_order_acquire))
        return 0;
    }
    return static_cast<int>(n);
  }

  // LOAD path (unchanged): header / dimension probe needs the bytes now, before the async stream
  // has spun up -- wait them out.
  size_t copied = 0;
  uint32_t waited = 0;
  while (copied < size) {
    size_t n = this->ring_->read(buffer + copied, size - copied, pdMS_TO_TICKS(WAIT_SLICE_MS));
    if (n > 0) {
      copied += n;
      this->current_position_ += n;
      waited = 0;
      this->kick_fill_();  // made room -- keep the stream fed
      continue;
    }
    if (this->fill_err_.load(std::memory_order_acquire))
      return copied > 0 ? static_cast<int>(copied) : -1;
    if (this->eof_.load(std::memory_order_acquire) && this->ring_->available() == 0)
      break;  // stream ended and ring drained
    if (this->abort_flag_ != nullptr && *this->abort_flag_)
      return copied > 0 ? static_cast<int>(copied) : -1;
    this->kick_fill_();
    if ((waited += WAIT_SLICE_MS) >= WAIT_CAP_MS)
      return copied > 0 ? static_cast<int>(copied) : -1;
  }
  return static_cast<int>(copied);
}

bool BufferedFileReader::seek(uint64_t position) {
  if (!this->open_)
    return false;
  this->quiesce_fill_();
  this->ring_->reset();
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
  if (!this->open_)
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
