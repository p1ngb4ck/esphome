#pragma once

#include "esphome/core/defines.h"

#ifdef USE_STORAGE_ISO9660

#include "storage.h"

#include <cstddef>
#include <cstdint>

namespace esphome::storage::iso9660 {

static constexpr uint32_t SECTOR_SIZE = 2048;
static constexpr size_t INDEX_MAX = 2 * 1024 * 1024;
static constexpr size_t LABEL_SIZE = 49;

struct Entry {
  uint32_t lba{0};
  uint64_t size{0};
  uint32_t mtime{0};
  bool is_dir{false};
  bool contiguous{true};
};

class Index {
 public:
  Index() = default;
  ~Index() { this->clear(); }
  Index(const Index &) = delete;
  Index &operator=(const Index &) = delete;

  void clear();
  bool empty() const { return this->dir_count_ == 0; }
  size_t bytes() const { return this->blob_len_ + this->dir_count_ * sizeof(DirRec); }
  uint64_t volume_bytes() const { return this->volume_bytes_; }
  const char *label() const { return this->label_; }

  StorageError lookup(const char *path, Entry *out) const;
  StorageError stat(const char *path, FileStat *st) const;
  StorageError list(const char *path, bool (*callback)(const FileStat *entry, void *ctx), void *ctx) const;

 protected:
  friend class Builder;

  struct DirRec {
    uint32_t lba;
    uint32_t size;
    uint32_t first;
    uint32_t count;
  };

  bool add_dir_(uint32_t lba, uint32_t size, uint32_t *index_out);
  uint8_t *add_entry_(size_t name_len);
  bool find_in_dir_(uint32_t dir, const char *name, size_t name_len, const uint8_t **rec_out) const;
  static void read_entry_(const uint8_t *rec, Entry *out, const char **name, uint8_t *name_len);
  void trim_();

  DirRec *dirs_{nullptr};
  uint32_t dir_count_{0};
  uint32_t dir_cap_{0};
  uint8_t *blob_{nullptr};
  size_t blob_len_{0};
  size_t blob_cap_{0};
  uint64_t volume_bytes_{0};
  char label_[LABEL_SIZE]{};
};

class Builder {
 public:
  enum class Step : uint8_t { READ, DONE, FAILED };

  void start(Index *index, uint32_t session_lba, uint32_t max_sectors);
  Step next(uint32_t *lba, uint32_t *count);
  void feed(const uint8_t *data, uint32_t count);
  StorageError error() const { return this->error_; }

 protected:
  enum class Phase : uint8_t { DESCRIPTORS, DIRECTORIES, DONE, FAILED };

  void fail_(StorageError err);
  void take_descriptor_(const uint8_t *vd);
  void begin_directories_();
  void parse_dir_sector_(const uint8_t *sector, uint32_t valid);
  void add_record_(const uint8_t *rec);

  Index *index_{nullptr};
  Phase phase_{Phase::FAILED};
  StorageError error_{StorageError::STORAGE_ERROR_OK};
  uint32_t session_lba_{0};
  uint32_t max_sectors_{1};
  uint32_t vd_next_{0};
  uint32_t requested_lba_{0};
  uint32_t requested_count_{0};
  uint32_t root_lba_{0};
  uint32_t root_size_{0};
  bool joliet_{false};
  bool have_primary_{false};
  uint32_t cur_dir_{0};
  uint32_t cur_done_{0};
  int64_t pending_multi_{-1};
};

}

#endif
