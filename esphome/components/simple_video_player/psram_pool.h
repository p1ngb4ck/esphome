#pragma once

#include <cstddef>
#include <cstdint>

#include <atomic>

namespace esphome::simple_video_player {

/// The player's PSRAM pieces, each taken from PSRAM (the system's allocator, MALLOC_CAP_SPIRAM only - never internal
/// RAM) when its function needs it and given back after; nothing is reserved. 128-byte aligned (whole cache lines,
/// as the JPEG unit and DMA need). Thread-safe (the system's allocator). size() is the media budget plan_pool_()
/// shares out, used() what the player holds now, largest_free() PSRAM's largest free block.
class PsramPool {
 public:
  static constexpr size_t ALIGN = 128;

  bool init(size_t size);
  bool ready() const { return this->ready_; }

  /// nullptr when PSRAM has no free block that large.
  uint8_t *alloc(size_t size);
  /// The same as alloc() (kept for the callers of the block's time).
  uint8_t *alloc_top(size_t size);
  void free(uint8_t *ptr);

  size_t size() const { return this->size_; }
  size_t used() const;
  size_t largest_free() const;

  /// The pool the player's objects come from (the first one made ready).
  static PsramPool *instance();

 protected:
  size_t size_{0};
  std::atomic<size_t> used_{0};
  bool ready_{false};
};

}  // namespace esphome::simple_video_player
