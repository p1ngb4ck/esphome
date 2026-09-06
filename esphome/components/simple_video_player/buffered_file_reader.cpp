/**
 * @file buffered_file_reader.cpp
 * @brief StorageWorker-backed file reader; read-ahead window is the shared
 *        storage::TransferBuffer arena (no allocation of our own).
 */

#include "buffered_file_reader.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "buffered_file_reader";

int BufferedFileReader::refill_() {
  size_t got = 0;
  this->arm_wait_();
  storage::StorageError submit = storage::global_storage_worker->read_chunk(
      this->handle_, this->window_, this->window_cap_, &got,
      [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "read_chunk failed to submit: %s", storage::error_to_string(submit));
    return -1;
  }
  storage::StorageError result = this->wait_();
  if (result != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "read_chunk failed: %s", storage::error_to_string(result));
    return -1;
  }
  this->win_pos_ = 0;
  this->win_len_ = got;
  this->file_pos_ += got;
  return static_cast<int>(got);
}

bool BufferedFileReader::open(const char *path) {
  if (this->open_) {
    this->close();
  }
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
  storage::StorageError submit = storage::global_storage_worker->begin_read(
      ps, rel, &this->handle_, [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "begin_read('%s') failed to submit: %s", path, storage::error_to_string(submit));
    return false;
  }
  if (this->wait_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "Failed to open '%s'", path);
    return false;
  }

  // Borrow the whole transfer arena as the read-ahead window for this session.
  this->window_ = storage::global_transfer_buffer->try_acquire(MIN_WINDOW);
  if (this->window_ == nullptr) {
    ESP_LOGE(TAG, "storage transfer buffer unavailable (busy or < %zu bytes)", MIN_WINDOW);
    this->arm_wait_();
    storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_done_(e); });
    this->wait_();
    return false;
  }
  this->window_cap_ = storage::global_transfer_buffer->capacity();

  this->open_ = true;
  this->current_position_ = 0;
  this->file_pos_ = 0;
  this->win_pos_ = 0;
  this->win_len_ = 0;
  return true;
}

void BufferedFileReader::close() {
  if (!this->open_) {
    return;
  }
  this->arm_wait_();
  storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_done_(e); });
  this->wait_();

  storage::global_transfer_buffer->release();
  this->window_ = nullptr;
  this->window_cap_ = 0;
  this->win_pos_ = 0;
  this->win_len_ = 0;
  this->file_pos_ = 0;
  this->open_ = false;
  this->current_position_ = 0;
}

int BufferedFileReader::read(uint8_t *buffer, size_t size) {
  if (!this->open_) {
    return -1;
  }
  size_t copied = 0;
  while (copied < size) {
    if (this->win_pos_ < this->win_len_) {
      size_t n = std::min(this->win_len_ - this->win_pos_, size - copied);
      std::memcpy(buffer + copied, this->window_ + this->win_pos_, n);
      this->win_pos_ += n;
      this->current_position_ += n;
      copied += n;
      continue;
    }
    int r = this->refill_();
    if (r < 0) {
      return copied > 0 ? static_cast<int>(copied) : -1;
    }
    if (r == 0) {
      break;  // EOF
    }
  }
  return static_cast<int>(copied);
}

bool BufferedFileReader::seek(uint64_t position) {
  if (!this->open_) {
    return false;
  }
  this->arm_wait_();
  storage::StorageError submit = storage::global_storage_worker->seek(
      this->handle_, static_cast<int64_t>(position), storage::SeekMode::SEEK_MODE_SET,
      [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "seek failed to submit: %s", storage::error_to_string(submit));
    return false;
  }
  if (this->wait_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "seek failed");
    return false;
  }
  this->current_position_ = position;
  this->file_pos_ = position;
  this->win_pos_ = 0;
  this->win_len_ = 0;  // lazy refill on next read()
  return true;
}

bool BufferedFileReader::get_size(uint64_t *size) {
  if (!this->open_) {
    return false;
  }
  uint64_t saved = this->current_position_;

  this->arm_wait_();
  if (storage::global_storage_worker->seek(this->handle_, 0, storage::SeekMode::SEEK_MODE_END,
                                           [this](storage::StorageError e) { this->on_done_(e); }) !=
          storage::StorageError::STORAGE_ERROR_OK ||
      this->wait_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: seek(END) failed");
    return false;
  }

  uint64_t end_pos = 0;
  this->arm_wait_();
  if (storage::global_storage_worker->tell(this->handle_, &end_pos,
                                           [this](storage::StorageError e) { this->on_done_(e); }) !=
          storage::StorageError::STORAGE_ERROR_OK ||
      this->wait_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: tell failed");
    return false;
  }

  if (!this->seek(saved)) {
    return false;
  }
  *size = end_pos;
  return true;
}

}  // namespace simple_video_player
}  // namespace esphome
