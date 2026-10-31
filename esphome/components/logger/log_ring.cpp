#include "log_ring.h"
#ifdef USE_LOGGER_LOG_RING

#include "logger.h"
#include "esphome/core/helpers.h"

#include <cstring>

namespace esphome::logger {

// A line in the ring: one byte, its level ('0' + level), then the text, then '\n'.

LogRing &LogRing::instance() {
  static LogRing ring;
  return ring;
}

bool LogRing::enable() {
  if (this->buf_ != nullptr)
    return true;
  if (global_logger == nullptr || this->size_ < 256)
    return false;
  RAMAllocator<char> alloc(RAMAllocator<char>::ALLOC_EXTERNAL);
  this->buf_ = alloc.allocate(this->size_);
  if (this->buf_ == nullptr)
    return false;
  global_logger->add_log_callback(this, [](void *self, uint8_t level, const char *, const char *message, size_t len) {
    static_cast<LogRing *>(self)->on_log_(level, message, len);
  });
  return true;
}

void LogRing::put_(char c) {
  this->buf_[this->head_] = c;
  this->head_ = (this->head_ + 1) % this->size_;
  if (this->used_ < this->size_)
    this->used_++;
}

void LogRing::on_log_(uint8_t level, const char *message, size_t len) {
  this->put_(static_cast<char>('0' + (level & 7)));
  // Without the colour codes (ESC '[' ... 'm') and line breaks inside the message
  for (size_t i = 0; i < len; i++) {
    const char c = message[i];
    if (c == '\033') {
      while (i < len && message[i] != 'm')
        i++;
      continue;
    }
    if (c == '\0')
      break;
    this->put_(c == '\n' || c == '\r' ? ' ' : c);
  }
  this->put_('\n');
  this->lines_++;
  if (this->listener_ != nullptr)
    this->listener_(this->listener_ctx_);
}

size_t LogRing::render(char *dst, size_t cap, uint8_t max_level) const {
  if (dst == nullptr || cap == 0)
    return 0;
  dst[0] = '\0';
  if (this->buf_ == nullptr || this->used_ == 0)
    return 0;
  const size_t start = (this->head_ + this->size_ - this->used_) % this->size_;
  auto at = [this, start](size_t i) { return this->buf_[(start + i) % this->size_]; };
  // When the ring has wrapped, its first line is cut: start after the first line break
  size_t first = 0;
  if (this->used_ == this->size_) {
    while (first < this->used_ && at(first) != '\n')
      first++;
    first++;
  }
  // Pass 1: the bytes of the lines shown, to skip the oldest that do not fit
  size_t total = 0;
  for (size_t i = first; i < this->used_;) {
    const uint8_t level = static_cast<uint8_t>(at(i) - '0');
    size_t end = i + 1;
    while (end < this->used_ && at(end) != '\n')
      end++;
    if (level <= max_level)
      total += end - i;  // text + '\n' (the level byte not copied)
    i = end + 1;
  }
  size_t skip = total > cap - 1 ? total - (cap - 1) : 0;
  // Pass 2: copy whole lines, skipping the oldest
  size_t out = 0;
  for (size_t i = first; i < this->used_;) {
    const uint8_t level = static_cast<uint8_t>(at(i) - '0');
    size_t end = i + 1;
    while (end < this->used_ && at(end) != '\n')
      end++;
    const size_t line = end - i;
    if (level <= max_level) {
      if (skip >= line) {
        skip -= line;
      } else {
        skip = 0;
        for (size_t k = i + 1; k <= end && k < this->used_ && out < cap - 1; k++)
          dst[out++] = at(k);
      }
    }
    i = end + 1;
  }
  dst[out] = '\0';
  return out;
}

}  // namespace esphome::logger

#endif  // USE_LOGGER_LOG_RING
