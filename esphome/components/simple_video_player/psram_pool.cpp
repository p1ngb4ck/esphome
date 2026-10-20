#include "psram_pool.h"

#include <cstring>

#include "esp_heap_caps.h"

namespace esphome::simple_video_player {

static PsramPool *s_instance = nullptr;

PsramPool *PsramPool::instance() { return s_instance; }

bool PsramPool::init(size_t size) {
  // Nothing is reserved: size is the media budget plan_pool_() shares out; every piece is taken from
  // PSRAM when its function needs it and given back after.
  this->size_ = size;
  this->ready_ = true;
  if (s_instance == nullptr)
    s_instance = this;
  return true;
}

uint8_t *PsramPool::alloc(size_t size) {
  if (!this->ready_ || size == 0)
    return nullptr;
  size = (size + ALIGN - 1) & ~(ALIGN - 1);
  // PSRAM only (no fallback into internal RAM), zeroed as the block was; one line before the piece holds its size
  auto *block = static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, size + ALIGN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (block == nullptr)
    return nullptr;
  memcpy(block, &size, sizeof(size));
  this->used_.fetch_add(size, std::memory_order_relaxed);
  return block + ALIGN;
}

uint8_t *PsramPool::alloc_top(size_t size) { return this->alloc(size); }

void PsramPool::free(uint8_t *ptr) {
  if (ptr == nullptr)
    return;
  uint8_t *block = ptr - ALIGN;
  size_t size = 0;
  memcpy(&size, block, sizeof(size));
  this->used_.fetch_sub(size, std::memory_order_relaxed);
  heap_caps_free(block);
}

size_t PsramPool::used() const { return this->used_.load(std::memory_order_relaxed); }

size_t PsramPool::largest_free() const { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }

}  // namespace esphome::simple_video_player
