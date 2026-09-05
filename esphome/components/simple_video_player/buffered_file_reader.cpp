/**
 * @file buffered_file_reader.cpp
 * @brief Implementation of the storage::StorageWorker-backed file reader
 */

#include "buffered_file_reader.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "buffered_file_reader";

BufferedFileReader::BufferedFileReader() {
#ifdef USE_ESP32
  this->done_sem_ = xSemaphoreCreateBinary();
#endif
}

BufferedFileReader::~BufferedFileReader() {
  this->close();
#ifdef USE_ESP32
  if (this->done_sem_ != nullptr) {
    vSemaphoreDelete(this->done_sem_);
  }
#endif
}

bool BufferedFileReader::open(const char *path) {
  if (this->open_) {
    ESP_LOGW(TAG, "File already open, closing first");
    this->close();
  }

  if (storage::global_storage_registry == nullptr || storage::global_storage_worker == nullptr) {
    ESP_LOGE(TAG, "Storage not available");
    return false;
  }

  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(path, &rel);
  if (ps == nullptr) {
    ESP_LOGE(TAG, "'%s' does not resolve to a mounted storage", path);
    return false;
  }

  storage::StorageError submit =
      storage::global_storage_worker->begin_read(ps, rel, &this->handle_, [this](storage::StorageError e) {
        this->on_done_(e);
      });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "begin_read('%s') failed to submit: %s", path, storage::error_to_string(submit));
    return false;
  }

  storage::StorageError result = this->wait_();
  if (result != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "Failed to open '%s': %s", path, storage::error_to_string(result));
    return false;
  }

  if (!this->read_ahead_buf_) {
    this->read_ahead_buf_.reset(
        static_cast<uint8_t *>(heap_caps_malloc(READ_AHEAD_CAPACITY, MALLOC_CAP_SPIRAM)));
    if (!this->read_ahead_buf_) {
      ESP_LOGE(TAG, "Failed to allocate %zu-byte read-ahead buffer (PSRAM)", READ_AHEAD_CAPACITY);
      storage::global_storage_worker->end_read(this->handle_,
                                               [this](storage::StorageError e) { this->on_done_(e); });
      this->wait_();
      return false;
    }
  }
  this->read_ahead_len_ = 0;
  this->read_ahead_pos_ = 0;

  this->open_ = true;
  this->current_position_ = 0;
  return true;
}

void BufferedFileReader::close() {
  if (!this->open_) {
    return;
  }

  storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_done_(e); });
  this->wait_();

  if (this->read_ahead_buf_) {
    heap_caps_free(this->read_ahead_buf_.release());
  }
  this->read_ahead_len_ = 0;
  this->read_ahead_pos_ = 0;

  this->open_ = false;
  this->current_position_ = 0;
}

int BufferedFileReader::read_chunk_(uint8_t *dest, size_t size) {
  size_t bytes_read = 0;
  storage::StorageError submit = storage::global_storage_worker->read_chunk(
      this->handle_, dest, size, &bytes_read, [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "read_chunk failed to submit: %s", storage::error_to_string(submit));
    return -1;
  }

  storage::StorageError result = this->wait_();
  if (result != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "read_chunk failed: %s", storage::error_to_string(result));
    return -1;
  }

  return static_cast<int>(bytes_read);
}

int BufferedFileReader::read(uint8_t *buffer, size_t size) {
  if (!this->open_) {
    return -1;
  }

  size_t total_copied = 0;

  while (total_copied < size) {
    // Serve from the read-ahead cache first.
    if (this->read_ahead_pos_ < this->read_ahead_len_) {
      size_t available = this->read_ahead_len_ - this->read_ahead_pos_;
      size_t to_copy = std::min(available, size - total_copied);
      std::memcpy(buffer + total_copied, this->read_ahead_buf_.get() + this->read_ahead_pos_, to_copy);
      this->read_ahead_pos_ += to_copy;
      this->current_position_ += to_copy;
      total_copied += to_copy;
      continue;
    }

    // Cache exhausted. A request too large to benefit from caching bypasses it with its own
    // direct read straight into the caller's buffer (this is the per-frame JPEG payload path).
    size_t remaining = size - total_copied;
    if (remaining >= READ_AHEAD_CAPACITY) {
      int n = this->read_chunk_(buffer + total_copied, remaining);
      if (n < 0) {
        return total_copied > 0 ? static_cast<int>(total_copied) : -1;
      }
      this->current_position_ += n;
      total_copied += n;
      if (static_cast<size_t>(n) < remaining) {
        break;  // short read -- EOF
      }
      continue;
    }

    // Refill the whole cache in one shot, then loop back to copy from it.
    int n = this->read_chunk_(this->read_ahead_buf_.get(), READ_AHEAD_CAPACITY);
    if (n < 0) {
      return total_copied > 0 ? static_cast<int>(total_copied) : -1;
    }
    this->read_ahead_len_ = static_cast<size_t>(n);
    this->read_ahead_pos_ = 0;
    if (n == 0) {
      break;  // EOF
    }
  }

  return static_cast<int>(total_copied);
}

bool BufferedFileReader::seek(uint64_t position) {
  if (!this->open_) {
    return false;
  }

  storage::StorageError submit =
      storage::global_storage_worker->seek(this->handle_, static_cast<int64_t>(position),
                                           storage::SeekMode::SEEK_MODE_SET,
                                           [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "seek failed to submit: %s", storage::error_to_string(submit));
    return false;
  }

  storage::StorageError result = this->wait_();
  if (result != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "seek failed: %s", storage::error_to_string(result));
    return false;
  }

  // The underlying stream cursor just moved -- any unconsumed read-ahead bytes no longer
  // correspond to what comes next, so discard them.
  this->read_ahead_len_ = 0;
  this->read_ahead_pos_ = 0;

  this->current_position_ = position;
  return true;
}

bool BufferedFileReader::get_size(uint64_t *size) {
  if (!this->open_) {
    return false;
  }

  uint64_t saved_position = this->current_position_;

  // No dedicated stat call on the worker's stream API -- seek to end, tell, then seek back.
  // (The worker's own SEEK_MODE_END on a network stream already resolves via file size
  // internally, so this is a worker-native pattern, not a workaround.)
  storage::StorageError submit =
      storage::global_storage_worker->seek(this->handle_, 0, storage::SeekMode::SEEK_MODE_END,
                                           [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: seek(END) failed to submit: %s", storage::error_to_string(submit));
    return false;
  }
  if (this->wait_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: seek(END) failed");
    return false;
  }

  uint64_t end_position = 0;
  submit = storage::global_storage_worker->tell(this->handle_, &end_position,
                                                [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: tell failed to submit: %s", storage::error_to_string(submit));
    return false;
  }
  if (this->wait_() != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "get_size: tell failed");
    return false;
  }

  if (!this->seek(saved_position)) {
    ESP_LOGE(TAG, "get_size: failed to restore position");
    return false;
  }

  *size = end_position;
  return true;
}

}  // namespace simple_video_player
}  // namespace esphome
