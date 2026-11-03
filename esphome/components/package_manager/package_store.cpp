#include "package_store.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include <algorithm>
#include <cstring>

#include <esp_cache.h>
#include <esp_mmu_map.h>
#include <esp_rom_crc.h>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::package_manager {

static const char *const TAG = "package_manager.store";
static const uint32_t STORE_MAGIC = 0x50534b31;
static const uint32_t FIRST_REGION_SECTOR = 2;
static const uint32_t CODE_VADDR = 0x43000000;

uint32_t PackageStore::crc_(const StoreIndex &index) const {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&index);
  return esp_rom_crc32_le(esp_rom_crc32_le(0, bytes, 12), bytes + 16, sizeof(StoreIndex) - 16);
}

bool PackageStore::open() {
  if (this->partition_ != nullptr)
    return true;
  const esp_partition_t *partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "packages");
  if (partition == nullptr) {
    ESP_LOGE(TAG, "no 'packages' partition");
    return false;
  }
  if (partition->address % 0x10000 != 0) {
    ESP_LOGE(TAG, "'packages' partition not 64 KB aligned");
    return false;
  }
  RAMAllocator<StoreIndex> allocator(RAMAllocator<StoreIndex>::ALLOC_EXTERNAL);
  StoreIndex *index = allocator.allocate(1);
  if (index == nullptr) {
    ESP_LOGE(TAG, "no PSRAM for the package index");
    return false;
  }
  uint32_t best_sequence = 0;
  int best = -1;
  for (uint8_t copy = 0; copy < 2; copy++) {
    if (esp_partition_read(partition, copy * STORE_SECTOR, index, sizeof(StoreIndex)) != ESP_OK)
      continue;
    if (index->magic != STORE_MAGIC || index->crc != this->crc_(*index))
      continue;
    if (best < 0 || index->sequence > best_sequence) {
      best = copy;
      best_sequence = index->sequence;
    }
  }
  if (best >= 0) {
    esp_partition_read(partition, best * STORE_SECTOR, index, sizeof(StoreIndex));
    this->copy_ = best;
  } else {
    std::memset(index, 0, sizeof(StoreIndex));
    index->magic = STORE_MAGIC;
    index->next = FIRST_REGION_SECTOR * STORE_SECTOR;
    this->copy_ = 1;
  }
  void *code = nullptr;
  esp_err_t err = esp_mmu_map_virt(CODE_VADDR, partition->address, partition->size, MMU_TARGET_FLASH0,
                                   static_cast<mmu_mem_caps_t>(MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ |
                                                               MMU_MEM_CAP_32BIT),
                                   0,
                                   &code);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mapping the 'packages' partition failed: %s", esp_err_to_name(err));
    allocator.deallocate(index, 1);
    return false;
  }
  this->partition_ = partition;
  this->index_ = index;
  this->code_ = static_cast<const uint8_t *>(code);
  this->sectors_ = std::min<uint32_t>(partition->size / STORE_SECTOR, STORE_MAX_SECTORS);
  return true;
}

StoreEntry *PackageStore::find(const char *name) {
  for (auto &entry : this->index_->entries) {
    if (entry.used && std::strncmp(entry.name, name, sizeof(entry.name)) == 0)
      return &entry;
  }
  return nullptr;
}

StoreEntry *PackageStore::free_entry() {
  for (auto &entry : this->index_->entries) {
    if (!entry.used)
      return &entry;
  }
  return nullptr;
}

bool PackageStore::region_free_(uint32_t first, uint32_t count, uint32_t skip_offset) const {
  if (first < FIRST_REGION_SECTOR || first + count > this->sectors_)
    return false;
  for (const auto &entry : this->index_->entries) {
    if (!entry.used || entry.offset == skip_offset)
      continue;
    const uint32_t start = entry.offset / STORE_SECTOR;
    const uint32_t end = start + (entry.size + STORE_SECTOR - 1) / STORE_SECTOR;
    if (first < end && start < first + count)
      return false;
  }
  return true;
}

bool PackageStore::allocate(uint32_t size, uint32_t &offset) {
  const uint32_t count = (size + STORE_SECTOR - 1) / STORE_SECTOR;
  if (count == 0 || count > this->sectors_ - FIRST_REGION_SECTOR)
    return false;
  const uint32_t span = this->sectors_ - FIRST_REGION_SECTOR;
  uint32_t start = this->index_->next / STORE_SECTOR;
  if (start < FIRST_REGION_SECTOR || start >= this->sectors_)
    start = FIRST_REGION_SECTOR;
  for (uint32_t step = 0; step < span; step++) {
    const uint32_t first = FIRST_REGION_SECTOR + (start - FIRST_REGION_SECTOR + step) % span;
    if (this->region_free_(first, count, UINT32_MAX)) {
      offset = first * STORE_SECTOR;
      this->index_->next = offset + count * STORE_SECTOR;
      this->erased_offset_ = UINT32_MAX;
      return true;
    }
  }
  return false;
}

bool PackageStore::erase_(uint32_t sector) {
  if (esp_partition_erase_range(this->partition_, sector * STORE_SECTOR, STORE_SECTOR) != ESP_OK)
    return false;
  if (sector < STORE_MAX_SECTORS && this->index_->erases[sector] != UINT16_MAX)
    this->index_->erases[sector]++;
  return true;
}

bool PackageStore::write(uint32_t offset, uint32_t position, const uint8_t *data, size_t len) {
  if (this->erased_offset_ != offset) {
    this->erased_offset_ = offset;
    this->erased_through_ = offset / STORE_SECTOR;
  }
  const uint32_t absolute = offset + position;
  while (this->erased_through_ * STORE_SECTOR < absolute + len) {
    if (!this->erase_(this->erased_through_))
      return false;
    this->erased_through_++;
  }
  return esp_partition_write(this->partition_, absolute, data, len) == ESP_OK;
}

bool PackageStore::commit() {
  StoreIndex &index = *this->index_;
  index.sequence++;
  const uint8_t target = this->copy_ ^ 1;
  if (!this->erase_(target))
    return false;
  index.crc = this->crc_(index);
  if (esp_partition_write(this->partition_, target * STORE_SECTOR, &index, sizeof(StoreIndex)) != ESP_OK)
    return false;
  this->copy_ = target;
  return true;
}

void PackageStore::sync_code(uint32_t offset, uint32_t size) {
  void *start = const_cast<uint8_t *>(this->code_ + offset);
  esp_cache_msync(start, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
  esp_cache_msync(start, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

}

#endif
