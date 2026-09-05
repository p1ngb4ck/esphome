/**
 * @file buffered_file_reader.cpp
 * @brief Implementation of the storage::StorageWorker-backed file reader
 */

#include "buffered_file_reader.h"
#include "esphome/core/log.h"

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

  this->open_ = false;
  this->current_position_ = 0;
}

int BufferedFileReader::read(uint8_t *buffer, size_t size) {
  if (!this->open_) {
    return -1;
  }

  size_t bytes_read = 0;
  storage::StorageError submit = storage::global_storage_worker->read_chunk(
      this->handle_, buffer, size, &bytes_read, [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "read_chunk failed to submit: %s", storage::error_to_string(submit));
    return -1;
  }

  storage::StorageError result = this->wait_();
  if (result != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "read_chunk failed: %s", storage::error_to_string(result));
    return -1;
  }

  this->current_position_ += bytes_read;
  return static_cast<int>(bytes_read);
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
