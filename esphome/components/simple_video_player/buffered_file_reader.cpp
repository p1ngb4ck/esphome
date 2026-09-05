/**
 * @file buffered_file_reader.cpp
 * @brief Implementation of the double-buffered, prefetching StorageWorker-backed file reader
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

void BufferedFileReader::start_prefetch_(int idx) {
  this->prefetch_idx_ = idx;
  this->prefetch_bytes_ = 0;
  storage::StorageError submit = storage::global_storage_worker->read_chunk(
      this->handle_, this->read_ahead_buf_[idx].get(), READ_AHEAD_CAPACITY, &this->prefetch_bytes_,
      [this](storage::StorageError e) { this->on_done_(e); });
  if (submit != storage::StorageError::STORAGE_ERROR_OK) {
    // NOT_READY here would mean the stream wasn't actually IDLE -- a caller-ordering bug (every
    // call site resolves any pending prefetch before doing anything else that touches the
    // stream), not a runtime condition to recover from. Mark as "nothing pending" so
    // resolve_prefetch_() doesn't block forever waiting for a completion that will never fire.
    ESP_LOGE(TAG, "prefetch read_chunk failed to submit: %s", storage::error_to_string(submit));
    this->prefetch_pending_ = false;
    this->read_ahead_len_[idx] = 0;
    return;
  }
  this->prefetch_pending_ = true;
}

void BufferedFileReader::resolve_prefetch_() {
  if (!this->prefetch_pending_) {
    return;
  }
  storage::StorageError result = this->wait_();
  this->read_ahead_len_[this->prefetch_idx_] = (result == storage::StorageError::STORAGE_ERROR_OK)
                                                   ? this->prefetch_bytes_
                                                   : 0;
  if (result != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "prefetch read_chunk failed: %s", storage::error_to_string(result));
  }
  this->prefetch_pending_ = false;
}

bool BufferedFileReader::swap_to_prefetched_() {
  this->resolve_prefetch_();

  int other = 1 - this->active_idx_;
  if (this->read_ahead_len_[other] == 0) {
    return false;  // EOF (or the prefetch failed) -- nothing more buffered to swap to
  }

  this->active_idx_ = other;
  this->read_ahead_pos_ = 0;

  // The buffer we just finished consuming is now free -- start refilling it for the round after
  // this one, overlapping that fetch with however long the caller takes to consume/decode what
  // we just swapped in. Only worth doing if the buffer we just swapped INTO wasn't already a
  // short read (EOF imminent): a short read means there's nothing beyond it to prefetch for.
  if (this->read_ahead_len_[other] == READ_AHEAD_CAPACITY) {
    this->start_prefetch_(1 - other);
  }
  return true;
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

  for (auto &buf : this->read_ahead_buf_) {
    if (!buf) {
      buf.reset(static_cast<uint8_t *>(heap_caps_malloc(READ_AHEAD_CAPACITY, MALLOC_CAP_SPIRAM)));
      if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu-byte read-ahead buffer (PSRAM)", READ_AHEAD_CAPACITY);
        storage::global_storage_worker->end_read(this->handle_,
                                                 [this](storage::StorageError e) { this->on_done_(e); });
        this->wait_();
        return false;
      }
    }
  }

  this->open_ = true;
  this->current_position_ = 0;
  this->prefetch_pending_ = false;

  // Prime buffer 0 synchronously (no way around paying for the very first fetch), then kick off
  // buffer 1's fetch immediately so it's already in flight before the caller ever asks for data.
  this->active_idx_ = 0;
  int n = this->read_chunk_(this->read_ahead_buf_[0].get(), READ_AHEAD_CAPACITY);
  if (n < 0) {
    ESP_LOGE(TAG, "Initial fill failed for '%s'", path);
    this->open_ = false;
    return false;
  }
  this->read_ahead_len_[0] = static_cast<size_t>(n);
  this->read_ahead_pos_ = 0;
  this->read_ahead_len_[1] = 0;
  if (this->read_ahead_len_[0] == READ_AHEAD_CAPACITY) {
    this->start_prefetch_(1);
  }

  return true;
}

void BufferedFileReader::close() {
  if (!this->open_) {
    return;
  }

  // end_read() needs the stream IDLE, same as any other stream op.
  this->resolve_prefetch_();

  storage::global_storage_worker->end_read(this->handle_, [this](storage::StorageError e) { this->on_done_(e); });
  this->wait_();

  for (auto &buf : this->read_ahead_buf_) {
    if (buf) {
      heap_caps_free(buf.release());
    }
  }
  this->read_ahead_len_[0] = 0;
  this->read_ahead_len_[1] = 0;
  this->read_ahead_pos_ = 0;
  this->active_idx_ = 0;

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
    // Serve from the active read-ahead buffer first.
    if (this->read_ahead_pos_ < this->read_ahead_len_[this->active_idx_]) {
      size_t available = this->read_ahead_len_[this->active_idx_] - this->read_ahead_pos_;
      size_t to_copy = std::min(available, size - total_copied);
      std::memcpy(buffer + total_copied, this->read_ahead_buf_[this->active_idx_].get() + this->read_ahead_pos_,
                 to_copy);
      this->read_ahead_pos_ += to_copy;
      this->current_position_ += to_copy;
      total_copied += to_copy;
      continue;
    }

    // Active buffer exhausted. Always swap onto whatever's already been prefetched (blocking
    // only if that fetch genuinely hasn't finished yet) rather than bypassing straight to a raw
    // direct read here -- a prefetch already in flight has, by definition, advanced the
    // UNDERLYING stream cursor past data the caller hasn't been given yet (prefetching reads the
    // bytes into a buffer; current_position_ only advances when they're actually copied out to
    // the caller). Resolving that prefetch and then reading further directly, without first
    // draining what it fetched, would silently skip over that whole buffer's worth of the file --
    // a real, serious bug a prior version of this function had for any request >= 1MB (this
    // player's own JPEG payload reads can plausibly hit that at this resolution). Requests larger
    // than one buffer just take multiple loop iterations (swap, drain, swap again) -- a few extra
    // PSRAM-to-caller memcpys for the rare oversized request, which is nothing next to correctness.
    if (!this->swap_to_prefetched_()) {
      break;  // EOF
    }
  }

  return static_cast<int>(total_copied);
}

bool BufferedFileReader::seek(uint64_t position) {
  if (!this->open_) {
    return false;
  }

  // seek() needs the stream IDLE, same as any other stream op.
  this->resolve_prefetch_();

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

  // Re-prime both buffers from the new position, same as open() does, so reads after a seek get
  // the same prefetch-ahead benefit instead of falling back to unbuffered one-shot fetches.
  this->active_idx_ = 0;
  int n = this->read_chunk_(this->read_ahead_buf_[0].get(), READ_AHEAD_CAPACITY);
  if (n < 0) {
    this->read_ahead_len_[0] = 0;
    this->read_ahead_len_[1] = 0;
    this->read_ahead_pos_ = 0;
    return false;
  }
  this->read_ahead_len_[0] = static_cast<size_t>(n);
  this->read_ahead_pos_ = 0;
  this->read_ahead_len_[1] = 0;
  if (this->read_ahead_len_[0] == READ_AHEAD_CAPACITY) {
    this->start_prefetch_(1);
  }

  return true;
}

bool BufferedFileReader::get_size(uint64_t *size) {
  if (!this->open_) {
    return false;
  }

  uint64_t saved_position = this->current_position_;

  // seek()/tell() need the stream IDLE, same as any other stream op.
  this->resolve_prefetch_();

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
