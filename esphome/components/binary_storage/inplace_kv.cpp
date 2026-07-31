#include "inplace_kv.h"

#ifdef USE_BINARY_STORAGE_INPLACE_KV

#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace binary_storage {

static const char *const TAG = "binary_storage.inplace_kv";

// Header: magic(4) version(1) reserved(3) region_size(4). Entries follow.
static const uint32_t IKV_MAGIC = 0x31564B49;  // "IKV1"
static const uint8_t IKV_VERSION = 1;
static const uint64_t HEADER_SIZE = 12;

// Entry: committed(1) live(1) key(4) len(2) value(len). The commit marker is written LAST.
static const uint64_t ENTRY_HDR = 8;
static const uint8_t COMMITTED = 0xA5;  // distinctive; 0 marks free space (format zeros the region)

// Minimum usable window. A KV store on a few dozen bytes is pointless.
static const uint64_t MIN_WINDOW = 256;

static uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static void wr_u32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t) p[0] | ((uint16_t) p[1] << 8); }
static void wr_u16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

bool InplaceKVStore::read_(uint64_t rel_offset, uint8_t *buf, size_t len) {
  if (this->device_ == nullptr || rel_offset + len > this->size_)
    return false;
  size_t done = 0;
  while (done < len) {
    size_t got = 0;
    if (this->device_->read(this->offset_ + rel_offset + done, buf + done, len - done, &got) !=
        storage::StorageError::OK)
      return false;
    if (got == 0)
      return false;  // unexpected EOF within the window
    done += got;
  }
  return true;
}

bool InplaceKVStore::write_(uint64_t rel_offset, const uint8_t *buf, size_t len) {
  if (this->device_ == nullptr || rel_offset + len > this->size_)
    return false;
  size_t done = 0;
  while (done < len) {
    size_t put = 0;
    if (this->device_->write(this->offset_ + rel_offset + done, buf + done, len - done, &put) !=
        storage::StorageError::OK)
      return false;
    if (put == 0)
      return false;
    done += put;
  }
  return true;
}

bool InplaceKVStore::zero_(uint64_t rel_offset, uint64_t len) {
  uint8_t chunk[64];
  memset(chunk, 0, sizeof(chunk));
  uint64_t done = 0;
  while (done < len) {
    size_t n = (size_t) ((len - done) < sizeof(chunk) ? (len - done) : sizeof(chunk));
    if (!this->write_(rel_offset + done, chunk, n))
      return false;
    done += n;
  }
  return true;
}

bool InplaceKVStore::header_valid_() {
  uint8_t hdr[HEADER_SIZE];
  if (!this->read_(0, hdr, HEADER_SIZE))
    return false;
  return rd_u32(hdr) == IKV_MAGIC && hdr[4] == IKV_VERSION;
}

bool InplaceKVStore::write_header_() {
  uint8_t hdr[HEADER_SIZE];
  memset(hdr, 0, sizeof(hdr));
  wr_u32(hdr, IKV_MAGIC);
  hdr[4] = IKV_VERSION;
  wr_u32(hdr + 8, (uint32_t) this->size_);
  return this->write_(0, hdr, HEADER_SIZE);
}

storage::StorageError InplaceKVStore::ensure_initialized() {
  if (this->initialized_)
    return storage::StorageError::OK;
  if (this->device_ == nullptr || this->size_ < MIN_WINDOW)
    return storage::StorageError::NOT_READY;
  // Refuse media that need erase -- this store is only correct on in-place (FRAM/MRAM) devices.
  storage::RawGeometry geo{};
  this->device_->get_raw_geometry(&geo);
  if ((geo.caps & storage::RAW_WRITE_NEEDS_ERASE) != 0) {
    ESP_LOGE(TAG, "Backing device needs erase; not an in-place medium");
    return storage::StorageError::NOT_SUPPORTED;
  }
  if (!this->header_valid_()) {
    // Empty or foreign medium: lay down a fresh store in place.
    ESP_LOGW(TAG, "No valid KV header, formatting window");
    storage::StorageError err = this->format();
    if (err != storage::StorageError::OK)
      return err;
  }
  this->initialized_ = true;
  return storage::StorageError::OK;
}

storage::StorageError InplaceKVStore::format() {
  // Zero the region first so committed==0 reliably marks free space, then write the header.
  if (!this->zero_(0, this->size_))
    return storage::StorageError::WRITE_ERROR;
  if (!this->write_header_())
    return storage::StorageError::WRITE_ERROR;
  this->initialized_ = true;
  return storage::StorageError::OK;
}

bool InplaceKVStore::find_(uint32_t key, uint64_t *entry_offset, uint16_t *value_len, uint64_t *entries_end) {
  uint64_t pos = HEADER_SIZE;
  bool found = false;
  uint64_t found_off = 0;
  uint16_t found_len = 0;
  while (pos + ENTRY_HDR <= this->size_) {
    uint8_t eh[ENTRY_HDR];
    if (!this->read_(pos, eh, ENTRY_HDR))
      break;
    if (eh[0] != COMMITTED)
      break;  // first free slot -> end of entries
    uint8_t live = eh[1];
    uint32_t ekey = rd_u32(eh + 2);
    uint16_t elen = rd_u16(eh + 6);
    uint64_t total = ENTRY_HDR + elen;
    if (pos + total > this->size_)
      break;  // truncated tail, ignore
    if (live == 1 && ekey == key) {  // last committed+live wins
      found = true;
      found_off = pos;
      found_len = elen;
    }
    pos += total;
  }
  if (entries_end != nullptr)
    *entries_end = pos;
  if (found) {
    if (entry_offset != nullptr)
      *entry_offset = found_off;
    if (value_len != nullptr)
      *value_len = found_len;
  }
  return found;
}

void InplaceKVStore::clear_live_before_(uint32_t key, uint64_t keep_offset) {
  uint64_t pos = HEADER_SIZE;
  while (pos + ENTRY_HDR <= this->size_ && pos < keep_offset) {
    uint8_t eh[ENTRY_HDR];
    if (!this->read_(pos, eh, ENTRY_HDR))
      break;
    if (eh[0] != COMMITTED)
      break;
    uint16_t elen = rd_u16(eh + 6);
    if (eh[1] == 1 && rd_u32(eh + 2) == key) {
      uint8_t dead = 0;
      this->write_(pos + 1, &dead, 1);  // clear the live byte
    }
    pos += ENTRY_HDR + elen;
  }
}

storage::StorageError InplaceKVStore::append_(uint32_t key, const uint8_t *data, size_t len) {
  uint64_t end = 0;
  this->find_(key, nullptr, nullptr, &end);
  uint64_t total = ENTRY_HDR + len;
  if (end + total > this->size_)
    return storage::StorageError::NO_SPACE;  // compaction is a separate step (see TODO phase 2b)

  // Write the body with committed=0, then set committed LAST so a torn write is ignored.
  uint8_t eh[ENTRY_HDR];
  eh[0] = 0;  // not yet committed
  eh[1] = 1;  // live
  wr_u32(eh + 2, key);
  wr_u16(eh + 6, (uint16_t) len);
  if (!this->write_(end, eh, ENTRY_HDR))
    return storage::StorageError::WRITE_ERROR;
  if (len > 0 && !this->write_(end + ENTRY_HDR, data, len))
    return storage::StorageError::WRITE_ERROR;
  uint8_t commit = COMMITTED;
  if (!this->write_(end, &commit, 1))  // commit marker (atomic single byte)
    return storage::StorageError::WRITE_ERROR;

  // New value is durable now; supersede older copies. If interrupted here, last-wins keeps reads
  // correct and the stale entries are reclaimable later.
  this->clear_live_before_(key, end);
  return storage::StorageError::OK;
}

storage::StorageError InplaceKVStore::set(uint32_t key, const uint8_t *data, size_t len) {
  if (len > 0xFFFF)
    return storage::StorageError::INVALID_ARGS;
  storage::StorageError err = this->ensure_initialized();
  if (err != storage::StorageError::OK)
    return err;
  return this->append_(key, data, len);
}

storage::StorageError InplaceKVStore::get(uint32_t key, uint8_t *buf, size_t len, size_t *got) {
  *got = 0;
  storage::StorageError err = this->ensure_initialized();
  if (err != storage::StorageError::OK)
    return err;
  uint64_t off = 0;
  uint16_t vlen = 0;
  if (!this->find_(key, &off, &vlen, nullptr))
    return storage::StorageError::NOT_FOUND;
  if (vlen > len)
    return storage::StorageError::INVALID_ARGS;  // query get_size() first
  if (vlen > 0 && !this->read_(off + ENTRY_HDR, buf, vlen))
    return storage::StorageError::READ_ERROR;
  *got = vlen;
  return storage::StorageError::OK;
}

storage::StorageError InplaceKVStore::get_size(uint32_t key, size_t *out) {
  *out = 0;
  storage::StorageError err = this->ensure_initialized();
  if (err != storage::StorageError::OK)
    return err;
  uint16_t vlen = 0;
  if (!this->find_(key, nullptr, &vlen, nullptr))
    return storage::StorageError::NOT_FOUND;
  *out = vlen;
  return storage::StorageError::OK;
}

bool InplaceKVStore::has(uint32_t key) {
  if (this->ensure_initialized() != storage::StorageError::OK)
    return false;
  return this->find_(key, nullptr, nullptr, nullptr);
}

storage::StorageError InplaceKVStore::erase(uint32_t key) {
  storage::StorageError err = this->ensure_initialized();
  if (err != storage::StorageError::OK)
    return err;
  uint64_t off = 0;
  if (!this->find_(key, &off, nullptr, nullptr))
    return storage::StorageError::OK;  // idempotent
  uint8_t dead = 0;
  if (!this->write_(off + 1, &dead, 1))
    return storage::StorageError::WRITE_ERROR;
  return storage::StorageError::OK;
}

storage::StorageError InplaceKVStore::get_info(storage::StorageInfo *info) {
  info->id = this->storage_id_;
  info->name = this->storage_name_ != nullptr ? this->storage_name_ : "inplace_kv";
  info->kind = "kv";
  info->total_bytes = this->size_;
  info->free_bytes = 0;
  info->block_size = 0;
  info->is_mounted = this->initialized_;
  info->is_removable = false;
  info->is_read_only = false;
  return storage::StorageError::OK;
}

void InplaceKVStore::setup() {
  if (this->ensure_initialized() != storage::StorageError::OK) {
    ESP_LOGE(TAG, "In-place KV init failed");
    this->mark_failed();
    return;
  }
  if (storage::global_storage_registry != nullptr) {
    if (storage::global_storage_registry->register_storage(this) != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Storage registration failed");
      this->mark_failed();
    }
  }
}

void InplaceKVStore::dump_config() {
  ESP_LOGCONFIG(TAG, "In-place key-value store:");
  ESP_LOGCONFIG(TAG, "  Window size: %llu bytes", (unsigned long long) this->size_);
}

}  // namespace binary_storage
}  // namespace esphome

#endif  // USE_BINARY_STORAGE_INPLACE_KV
