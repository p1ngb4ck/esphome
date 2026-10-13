#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace esphome::simple_video_player {

/// One PSRAM block for everything the player buffers, taken at boot. Pieces are handed out first
/// fit, 128-byte aligned (whole cache lines, as the JPEG unit and DMA need), and given back
/// whole; neighbouring free pieces merge. Thread-safe.
class PsramPool {
 public:
  static constexpr size_t ALIGN = 128;

  bool init(size_t size);
  bool ready() const { return this->base_ != nullptr; }

  /// nullptr when no free piece is large enough.
  uint8_t *alloc(size_t size);
  void free(uint8_t *ptr);

  size_t size() const { return this->size_; }
  size_t used() const;
  size_t largest_free() const;

 protected:
  struct Piece {
    size_t offset;
    size_t size;
    bool used;
  };
  uint8_t *base_{nullptr};
  size_t size_{0};
  std::vector<Piece> pieces_;  // in address order
  SemaphoreHandle_t lock_{nullptr};
};

}  // namespace esphome::simple_video_player
