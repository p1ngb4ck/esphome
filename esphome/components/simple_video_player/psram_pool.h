#pragma once

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace esphome::simple_video_player {

/// One PSRAM block for everything the player buffers, taken at boot. Pieces are handed out first
/// fit, 128-byte aligned (whole cache lines, as the JPEG unit and DMA need), and given back
/// whole; neighbouring free pieces merge. Thread-safe. Its piece table lives at the block's start
/// (no internal heap); with the table full a free piece is handed out whole instead of split.
class PsramPool {
 public:
  static constexpr size_t ALIGN = 128;
  static constexpr size_t MAX_PIECES = 128;

  bool init(size_t size);
  bool ready() const { return this->base_ != nullptr; }

  /// nullptr when no free piece is large enough.
  uint8_t *alloc(size_t size);
  /// The same from the pool's end (last fit): the player's small objects, so they never split the
  /// stretch the media buffers take and give back.
  uint8_t *alloc_top(size_t size);
  void free(uint8_t *ptr);

  size_t size() const { return this->size_; }
  size_t used() const;
  size_t largest_free() const;

  /// The pool the player's objects come from (the first one made ready).
  static PsramPool *instance();

 protected:
  struct Piece {
    size_t offset;
    size_t size;
    bool used;
  };
  void erase_(size_t index);  // lock held
  uint8_t *base_{nullptr};
  size_t size_{0};             // usable bytes, after the piece table
  Piece *pieces_{nullptr};     // in address order, at base_
  size_t count_{0};
  SemaphoreHandle_t lock_{nullptr};
};

}  // namespace esphome::simple_video_player
