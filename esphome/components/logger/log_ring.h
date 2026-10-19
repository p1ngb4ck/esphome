#pragma once

#include "esphome/core/defines.h"
#ifdef USE_LOGGER_LOG_RING

#include <cstddef>
#include <cstdint>

namespace esphome::logger {

/// The device's recent log lines in one PSRAM ring for viewers (e.g. a log page): any number of readers,
/// none consumes it. Taken on the first enable() and kept; nothing is held before. Each line is stored
/// as its level and its text without colour codes; the oldest lines go when it is full. Lines arrive on
/// the main loop (the task log buffer delivers other tasks' lines there), so does the listener.
class LogRing {
 public:
  static LogRing &instance();

  void set_size(size_t size) { this->size_ = size; }
  /// Allocates the ring (PSRAM only) and starts keeping lines; false without PSRAM.
  bool enable();
  bool enabled() const { return this->buf_ != nullptr; }
  /// Called after each kept line (main loop); one listener (the viewer coalesces its redraws).
  void set_listener(void (*fn)(void *), void *ctx) {
    this->listener_ = fn;
    this->listener_ctx_ = ctx;
  }
  /// The newest lines with a level up to `max_level` (ESPHOME_LOG_LEVEL_*) that fit into `cap - 1`
  /// bytes, oldest first, NUL terminated. Returns the length.
  size_t render(char *dst, size_t cap, uint8_t max_level) const;
  /// Lines kept since enable().
  uint32_t line_count() const { return this->lines_; }

 protected:
  void on_log_(uint8_t level, const char *message, size_t len);
  void put_(char c);

  char *buf_{nullptr};
  size_t size_{64 * 1024};
  size_t head_{0};  // next byte written
  size_t used_{0};  // bytes held (<= size_)
  uint32_t lines_{0};
  void (*listener_)(void *){nullptr};
  void *listener_ctx_{nullptr};
};

}  // namespace esphome::logger

#endif  // USE_LOGGER_LOG_RING
