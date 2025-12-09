/**
 * @file buffered_file_reader.cpp
 * @brief Implementation of optimized buffered file reader
 */

#include "buffered_file_reader.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstring>
#include <unistd.h>

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "buffered_file_reader";

// Helper macro for alignment
#define ALIGN_DOWN(num, align) ((num) & ~((align) -1))

BufferedFileReader::BufferedFileReader() {
  // Allocate cache buffer with DMA alignment for optimal SD card performance
#ifdef USE_ESP32
  this->cache_buffer_.reset(
      static_cast<uint8_t *>(heap_caps_aligned_alloc(128, CACHE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#else
  this->cache_buffer_.reset(new uint8_t[CACHE_SIZE]);
#endif

  if (!this->cache_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate %zu byte cache buffer", CACHE_SIZE);
  }
}

BufferedFileReader::~BufferedFileReader() { this->close(); }

bool BufferedFileReader::open(const char *path) {
  if (this->file_handle_ != nullptr) {
    ESP_LOGW(TAG, "File already open, closing first");
    this->close();
  }

  if (!this->cache_buffer_) {
    ESP_LOGE(TAG, "Cache buffer not allocated");
    return false;
  }

  // Open file using standard fopen (works with VFS-mounted storage)
  this->file_handle_ = std::fopen(path, "rb");
  if (this->file_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return false;
  }

  // Reset state
  this->flush_cache();
  this->current_position_ = 0;
  this->align_position_ = 0;
  this->eof_ = false;

  ESP_LOGD(TAG, "Opened file: %s", path);
  return true;
}

void BufferedFileReader::close() {
  if (this->file_handle_ != nullptr) {
    std::fclose(this->file_handle_);
    this->file_handle_ = nullptr;
    ESP_LOGD(TAG, "File closed");
  }

  this->flush_cache();
}

void BufferedFileReader::flush_cache() {
  this->cache_filled_ = 0;
  this->cache_offset_ = 0;
  this->cache_file_pos_ = 0;
  this->eof_ = false;
}

int BufferedFileReader::fill_cache_() {
  if (this->file_handle_ == nullptr) {
    return -1;
  }

  // Use read() instead of fread() for better control and alignment
  int fd = fileno(this->file_handle_);
  if (fd < 0) {
    ESP_LOGE(TAG, "Invalid file descriptor");
    return -1;
  }

  // Read full cache chunk
  ssize_t bytes_read = ::read(fd, this->cache_buffer_.get(), CACHE_SIZE);

  if (bytes_read < 0) {
    ESP_LOGE(TAG, "Read error: %d", errno);
    return -1;
  }

  if (bytes_read < static_cast<ssize_t>(CACHE_SIZE)) {
    this->eof_ = true;
  }

  this->cache_filled_ = bytes_read;
  this->cache_offset_ = 0;

  ESP_LOGV(TAG, "Filled cache: %zu bytes (EOF: %d)", this->cache_filled_, this->eof_);

  return bytes_read;
}

int BufferedFileReader::read_from_cache_(uint8_t *buffer, size_t size) {
  size_t total_read = 0;

  while (size > 0) {
    // Check if we need to refill cache
    if (this->cache_offset_ >= this->cache_filled_) {
      // Cache exhausted, check if EOF
      if (this->eof_) {
        break;  // No more data
      }

      // Update cache file position before refilling
      this->cache_file_pos_ += this->cache_filled_;

      // Refill cache
      int bytes_read = this->fill_cache_();
      if (bytes_read <= 0) {
        // Error or EOF with no data
        return (total_read > 0) ? static_cast<int>(total_read) : bytes_read;
      }
    }

    // Calculate available data in cache
    size_t available = this->cache_filled_ - this->cache_offset_;
    size_t to_copy = std::min(size, available);

    // Copy from cache to output buffer
    std::memcpy(buffer, this->cache_buffer_.get() + this->cache_offset_, to_copy);

    // Update pointers
    buffer += to_copy;
    this->cache_offset_ += to_copy;
    this->current_position_ += to_copy;
    size -= to_copy;
    total_read += to_copy;
  }

  return static_cast<int>(total_read);
}

int BufferedFileReader::read(uint8_t *buffer, size_t size) {
  if (this->file_handle_ == nullptr) {
    return -1;
  }

  // Handle seek offset if needed (when current_position_ != align_position_ + cache_offset_)
  // This happens after a seek that lands mid-cache
  if (this->cache_filled_ > 0) {
    // Check if current position is within cached data
    uint64_t cache_start = this->cache_file_pos_;
    uint64_t cache_end = this->cache_file_pos_ + this->cache_filled_;

    if (this->current_position_ >= cache_start && this->current_position_ < cache_end) {
      // Position is in cache, adjust offset
      this->cache_offset_ = this->current_position_ - cache_start;
    } else {
      // Position is outside cache, need to seek and refill
      this->flush_cache();

      // Align seek position to ALIGN_SIZE boundary
      this->align_position_ = ALIGN_DOWN(this->current_position_, ALIGN_SIZE);
      this->cache_file_pos_ = this->align_position_;

      if (std::fseek(this->file_handle_, this->align_position_, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to seek to aligned position %llu", this->align_position_);
        return -1;
      }

      // Fill cache
      int bytes_read = this->fill_cache_();
      if (bytes_read <= 0) {
        return bytes_read;
      }

      // Adjust offset to skip to current position
      size_t skip = this->current_position_ - this->align_position_;
      if (skip >= this->cache_filled_) {
        ESP_LOGE(TAG, "Seek skip too large: %zu >= %zu", skip, this->cache_filled_);
        return -1;
      }
      this->cache_offset_ = skip;
    }
  }

  return this->read_from_cache_(buffer, size);
}

bool BufferedFileReader::seek(uint64_t position) {
  if (this->file_handle_ == nullptr) {
    return false;
  }

  // Check if seek target is within current cache
  if (this->cache_filled_ > 0) {
    uint64_t cache_start = this->cache_file_pos_;
    uint64_t cache_end = this->cache_file_pos_ + this->cache_filled_;

    if (position >= cache_start && position < cache_end) {
      // Target is in cache, just adjust offset
      this->cache_offset_ = position - cache_start;
      this->current_position_ = position;
      ESP_LOGV(TAG, "Seek within cache to %llu (offset: %zu)", position, this->cache_offset_);
      return true;
    }
  }

  // Seek outside cache - flush and prepare for aligned read
  this->flush_cache();

  // Set current position (will be adjusted on next read)
  this->current_position_ = position;

  // Calculate aligned position for next read
  this->align_position_ = ALIGN_DOWN(position, ALIGN_SIZE);
  this->cache_file_pos_ = this->align_position_;

  // Perform physical seek to aligned position
  if (std::fseek(this->file_handle_, this->align_position_, SEEK_SET) != 0) {
    ESP_LOGE(TAG, "Failed to seek to position %llu (aligned: %llu)", position, this->align_position_);
    return false;
  }

  ESP_LOGV(TAG, "Seek to %llu (aligned: %llu)", position, this->align_position_);
  return true;
}

bool BufferedFileReader::get_size(uint64_t *size) {
  if (this->file_handle_ == nullptr) {
    return false;
  }

  long current = std::ftell(this->file_handle_);
  if (current < 0) {
    return false;
  }

  if (std::fseek(this->file_handle_, 0, SEEK_END) != 0) {
    return false;
  }

  long end = std::ftell(this->file_handle_);
  if (end < 0) {
    std::fseek(this->file_handle_, current, SEEK_SET);
    return false;
  }

  // Restore original position
  if (std::fseek(this->file_handle_, current, SEEK_SET) != 0) {
    return false;
  }

  *size = static_cast<uint64_t>(end);
  return true;
}

}  // namespace simple_video_player
}  // namespace esphome
