#include "transfer_buffer.h"
#ifdef USE_STORAGE_TRANSFER_BUFFER

#include <esp_heap_caps.h>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::storage {

static const char *const TAG = "storage.transfer_buffer";

TransferBuffer *global_transfer_buffer = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void TransferBuffer::setup() {
  // size 0 = auto (25% of the detected PSRAM); an explicit size must leave at least 20%
  // of PSRAM to the rest of the system. Both checks live here because the real PSRAM
  // size is only known after boot detection — config time cannot compute percentages.
  size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (total == 0) {
    ESP_LOGE(TAG, "no PSRAM detected — transfer buffer disabled");
    this->mark_failed();
    global_transfer_buffer = this;
    return;
  }
  if (this->size_ == 0) {
    this->size_ = total / 4;
  } else if (this->size_ > (total / 5) * 4) {
    ESP_LOGE(TAG, "configured size %u exceeds 80%% of PSRAM (%u of %u) — transfer buffer disabled",
             (unsigned) this->size_, (unsigned) ((total / 5) * 4), (unsigned) total);
    this->mark_failed();
    global_transfer_buffer = this;
    return;
  }
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
