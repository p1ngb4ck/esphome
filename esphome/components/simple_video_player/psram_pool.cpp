#include "psram_pool.h"

#include "driver/jpeg_decode.h"

namespace esphome::simple_video_player {

static PsramPool *s_instance = nullptr;

PsramPool *PsramPool::instance() { return s_instance; }

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
  const size_t table = (MAX_PIECES * sizeof(Piece) + ALIGN - 1) & ~(ALIGN - 1);
  const size_t total = got & ~(ALIGN - 1);
  if (total <= table)
    return false;
  this->pieces_ = reinterpret_cast<Piece *>(this->base_);
  this->size_ = total - table;
  this->pieces_[0] = {table, this->size_, false};
  this->count_ = 1;
  if (s_instance == nullptr)
    s_instance = this;
  return true;
}

uint8_t *PsramPool::alloc(size_t size) {
  if (this->base_ == nullptr || size == 0)
    return nullptr;
  size = (size + ALIGN - 1) & ~(ALIGN - 1);
  uint8_t *result = nullptr;
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  for (size_t i = 0; i < this->count_; i++) {
    Piece &piece = this->pieces_[i];
    if (piece.used || piece.size < size)
      continue;
    if (piece.size > size && this->count_ < MAX_PIECES) {
      for (size_t j = this->count_; j > i + 1; j--)
        this->pieces_[j] = this->pieces_[j - 1];
      this->pieces_[i + 1] = {piece.offset + size, piece.size - size, false};
      this->count_++;
      piece.size = size;
    }
    piece.used = true;
    result = this->base_ + piece.offset;
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
  for (size_t i = 0; i < this->count_; i++) {
    if (this->pieces_[i].offset != offset || !this->pieces_[i].used)
      continue;
    this->pieces_[i].used = false;
    if (i + 1 < this->count_ && !this->pieces_[i + 1].used) {
      this->pieces_[i].size += this->pieces_[i + 1].size;
      this->erase_(i + 1);
    }
    if (i > 0 && !this->pieces_[i - 1].used) {
      this->pieces_[i - 1].size += this->pieces_[i].size;
      this->erase_(i);
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
  for (size_t i = 0; i < this->count_; i++) {
    if (this->pieces_[i].used)
      total += this->pieces_[i].size;
  }
  xSemaphoreGive(this->lock_);
  return total;
}

size_t PsramPool::largest_free() const {
  size_t largest = 0;
  if (this->base_ == nullptr)
    return 0;
  xSemaphoreTake(this->lock_, portMAX_DELAY);
  for (size_t i = 0; i < this->count_; i++) {
    if (!this->pieces_[i].used && this->pieces_[i].size > largest)
      largest = this->pieces_[i].size;
  }
  xSemaphoreGive(this->lock_);
  return largest;
}

void PsramPool::erase_(size_t index) {
  for (size_t j = index; j + 1 < this->count_; j++)
    this->pieces_[j] = this->pieces_[j + 1];
  this->count_--;
}

}  // namespace esphome::simple_video_player
