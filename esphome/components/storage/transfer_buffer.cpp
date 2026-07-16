#include "transfer_buffer.h"
#ifdef USE_STORAGE_TRANSFER_BUFFER

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::storage {

static const char *const TAG = "storage.transfer_buffer";

TransferBuffer *global_transfer_buffer = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void TransferBuffer::setup() {
  // External-only on purpose: a multi-MB arena must never silently land in internal RAM.
  // Config validation already requires the psram component; allocation can still fail
  // (fragmentation, other consumers) — then the buffer stays disabled and every consumer
  // keeps streaming, which is the documented fallback.
  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  this->buf_ = allocator.allocate(this->size_);
  if (this->buf_ == nullptr) {
    ESP_LOGW(TAG, "PSRAM allocation of %u bytes failed — transfer buffer disabled, transfers stream directly",
             (unsigned) this->size_);
  }
  global_transfer_buffer = this;
}

void TransferBuffer::dump_config() {
  ESP_LOGCONFIG(TAG, "Transfer buffer:");
  ESP_LOGCONFIG(TAG, "  Size: %u bytes (%s)", (unsigned) this->size_,
                this->buf_ != nullptr ? "allocated in PSRAM" : "ALLOCATION FAILED — disabled");
}

uint8_t *TransferBuffer::try_acquire(size_t need) {
  if (this->buf_ == nullptr || need > this->size_)
    return nullptr;
  bool expected = false;
  if (!this->busy_.compare_exchange_strong(expected, true))
    return nullptr;  // someone else holds it — caller streams instead
  return this->buf_;
}

void TransferBuffer::release() { this->busy_.store(false); }

}  // namespace esphome::storage
#endif  // USE_STORAGE_TRANSFER_BUFFER
