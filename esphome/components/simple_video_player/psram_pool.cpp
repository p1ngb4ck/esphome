#include "psram_pool.h"

#include "driver/jpeg_decode.h"

namespace esphome::simple_video_player {

bool PsramPool::init(size_t size) {
  this->lock_ = xSemaphoreCreateMutex();
  if (this->lock_ == nullptr)
    return false;
  // The JPEG driver's allocator: PSRAM the JPEG unit and DMA take, on whole cache lines.
  jpeg_decode_memory_alloc_cfg_t cfg{};
  cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  size_t got = 0;
  this->base_ = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(size, &cfg, &got));
  if (this->base_ == nullptr)
    return false;
  this->size_ = got & ~(ALIGN - 1);
  this->pieces_.reserve(32);
  this->pieces_.push_back({0, this->size_, false});
  return true;
}

uint8_t *PsramPool::alloc(size_t size) {
  if (this->base_ == nullptr || size == 0)
    return nullptr;
  size = (size + ALIGN - 1) & ~(ALIGN - 1);
  uint8_t *result = nullptr;
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  for (size_t i = 0; i < this->pieces_.size(); i++) {
    Piece &piece = this->pieces_[i];
    if (piece.used || piece.size < size)
      continue;
    if (piece.size > size)
      this->pieces_.insert(this->pieces_.begin() + i + 1, {piece.offset + size, piece.size - size, false});
    Piece &taken = this->pieces_[i];
    taken.size = size;
    taken.used = true;
    result = this->base_ + taken.offset;
    break;
  }
  xSemaphoreGive(this->lock_);
  return result;
}

void PsramPool::free(uint8_t *ptr) {
  if (ptr == nullptr || this->base_ == nullptr)
    return;
  const size_t offset = static_cast<size_t>(ptr - this->base_);
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  for (size_t i = 0; i < this->pieces_.size(); i++) {
    if (this->pieces_[i].offset != offset || !this->pieces_[i].used)
      continue;
    this->pieces_[i].used = false;
    if (i + 1 < this->pieces_.size() && !this->pieces_[i + 1].used) {
      this->pieces_[i].size += this->pieces_[i + 1].size;
      this->pieces_.erase(this->pieces_.begin() + i + 1);
    }
    if (i > 0 && !this->pieces_[i - 1].used) {
      this->pieces_[i - 1].size += this->pieces_[i].size;
      this->pieces_.erase(this->pieces_.begin() + i);
    }
    break;
  }
  xSemaphoreGive(this->lock_);
}

size_t PsramPool::used() const {
  size_t total = 0;
  if (this->base_ == nullptr)
    return 0;
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  for (const Piece &piece : this->pieces_) {
    if (piece.used)
      total += piece.size;
  }
  xSemaphoreGive(this->lock_);
  return total;
}

size_t PsramPool::largest_free() const {
  size_t largest = 0;
  if (this->base_ == nullptr)
    return 0;
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  for (const Piece &piece : this->pieces_) {
    if (!piece.used && piece.size > largest)
      largest = piece.size;
  }
  xSemaphoreGive(this->lock_);
  return largest;
}

}  // namespace esphome::simple_video_player
