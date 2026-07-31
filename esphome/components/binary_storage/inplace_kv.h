#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BINARY_STORAGE_INPLACE_KV

#include "esphome/core/component.h"
#include "esphome/components/storage/storage.h"

namespace esphome {
namespace binary_storage {

// A KeyValueStorage for byte-addressable, erase-free non-volatile memory (FRAM, MRAM). It writes
// values in place -- no erase, no wear leveling, no flash-style garbage collection -- and is chosen
// purely by capability: the backing device must NOT advertise RAW_WRITE_NEEDS_ERASE. Flash and
// EEPROM are handled elsewhere.
//
// Layout on the medium is a header followed by variable-length entries packed in sequence. An entry
// is only considered valid once its trailing commit marker is written, so a torn write (power loss
// mid-write) leaves the entry ignored rather than corrupt. Updates append a new committed entry and
// then clear the previous one; a reader takes the LAST committed+live entry for a key, so even an
// update interrupted after commit but before the old entry is cleared still reads correctly.
class InplaceKVStore : public storage::KeyValueStorage {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // The backing byte device and the window [offset, offset+size) within it the store may use.
  void set_device(storage::RawStorage *device) { this->device_ = device; }
  void set_window(uint64_t offset, uint64_t size) {
    this->offset_ = offset;
    this->size_ = size;
  }
  void set_storage_id(const char *id) { this->storage_id_ = id; }
  void set_storage_name(const char *name) { this->storage_name_ = name; }

  storage::StorageError get_info(storage::StorageInfo *info) override;

  storage::StorageError get(uint32_t key, uint8_t *buf, size_t len, size_t *got) override;
  storage::StorageError set(uint32_t key, const uint8_t *data, size_t len) override;
  storage::StorageError erase(uint32_t key) override;
  bool has(uint32_t key) override;
  storage::StorageError get_size(uint32_t key, size_t *out) override;
  storage::StorageError ensure_initialized() override;
  storage::StorageError format() override;

 protected:
  // Byte access into the window (offsets are relative to the window start).
  bool read_(uint64_t rel_offset, uint8_t *buf, size_t len);
  bool write_(uint64_t rel_offset, const uint8_t *buf, size_t len);
  bool zero_(uint64_t rel_offset, uint64_t len);

  bool header_valid_();
  bool write_header_();

  // Locate the last committed+live entry for key. Returns true and fills *entry_offset / *value_len
  // when found. entries_end (first free offset) is filled whenever provided.
  bool find_(uint32_t key, uint64_t *entry_offset, uint16_t *value_len, uint64_t *entries_end);
  storage::StorageError append_(uint32_t key, const uint8_t *data, size_t len);
  void clear_live_before_(uint32_t key, uint64_t keep_offset);

  storage::RawStorage *device_{nullptr};
  uint64_t offset_{0};
  uint64_t size_{0};
  const char *storage_id_{nullptr};
  const char *storage_name_{nullptr};
  bool initialized_{false};
};

}  // namespace binary_storage
}  // namespace esphome

#endif  // USE_BINARY_STORAGE_INPLACE_KV
