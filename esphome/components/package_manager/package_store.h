#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include <cstddef>
#include <cstdint>

#include <esp_partition.h>

namespace esphome::package_manager {

enum class SettingsTarget : uint8_t { PREFERENCES, STORAGE };

static constexpr size_t STORE_SECTOR = 0x1000;
static constexpr size_t STORE_MAX_ENTRIES = 15;
static constexpr size_t STORE_MAX_SECTORS = 1000;

struct StoreEntry {
  char name[24];
  char version[16];
  char depends[60];
  uint32_t abi;
  uint32_t offset;
  uint32_t size;
  uint32_t length;
  uint32_t psram;
  uint32_t iram;
  uint8_t used;
  uint8_t enabled;
  uint8_t recovery;
  SettingsTarget settings;
};

static_assert(sizeof(StoreEntry) == 128, "StoreEntry is the flash index layout");

struct StoreIndex {
  uint32_t magic;
  uint32_t sequence;
  uint32_t next;
  uint32_t crc;
  StoreEntry entries[STORE_MAX_ENTRIES];
  uint16_t erases[STORE_MAX_SECTORS];
};

class PackageStore {
 public:
  bool open();
  bool is_open() const { return this->partition_ != nullptr; }
  StoreIndex &index() { return *this->index_; }
  const uint8_t *code_base() const { return this->code_; }
  uint32_t size() const { return this->partition_ != nullptr ? this->partition_->size : 0; }

  StoreEntry *find(const char *name);
  StoreEntry *free_entry();
  bool allocate(uint32_t size, uint32_t &offset);
  bool write(uint32_t offset, uint32_t position, const uint8_t *data, size_t len);
  bool commit();
  void sync_code(uint32_t offset, uint32_t size);

 protected:
  bool region_free_(uint32_t first, uint32_t count, uint32_t skip_offset) const;
  bool erase_(uint32_t sector);
  uint32_t crc_(const StoreIndex &index) const;

  const esp_partition_t *partition_{nullptr};
  StoreIndex *index_{nullptr};
  uint8_t copy_{0};
  uint32_t sectors_{0};
  uint32_t erased_through_{0};
  uint32_t erased_offset_{UINT32_MAX};
  const uint8_t *code_{nullptr};
};

}

#endif
