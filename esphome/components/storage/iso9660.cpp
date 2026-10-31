#include "iso9660.h"

#ifdef USE_STORAGE_ISO9660

#include "esphome/core/helpers.h"

#include <cstring>

namespace esphome::storage::iso9660 {

namespace {

constexpr size_t REC_HEAD = 18;
constexpr uint8_t FLAG_DIR = 1 << 0;
constexpr uint8_t FLAG_FRAGMENTED = 1 << 1;
constexpr uint8_t FLAG_OPEN_MULTI = 1 << 2;
constexpr uint32_t MAX_DESCRIPTORS = 32;
constexpr uint32_t DIR_SIZE_MAX = 4 * INDEX_MAX;
constexpr size_t BLOB_START = 4096;
constexpr uint32_t DIRS_START = 16;

template<typename T> T *psram_grow(T *ptr, size_t n) {
  return RAMAllocator<T>(RAMAllocator<T>::ALLOC_EXTERNAL).reallocate(ptr, n);
}

template<typename T> void psram_release(T *ptr) {
  if (ptr != nullptr)
    RAMAllocator<T>(RAMAllocator<T>::ALLOC_EXTERNAL).deallocate(ptr, 0);
}

uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

uint32_t record_time(const uint8_t *t) {
  if (t[1] < 1 || t[1] > 12 || t[2] < 1 || t[2] > 31)
    return 0;
  int64_t secs = days_from_civil(1900 + t[0], t[1], t[2]) * 86400 + t[3] * 3600 + t[4] * 60 + t[5];
  secs -= static_cast<int64_t>(static_cast<int8_t>(t[6])) * 900;
  if (secs < 0 || secs > static_cast<int64_t>(UINT32_MAX))
    return 0;
  return static_cast<uint32_t>(secs);
}

size_t put_utf8(uint32_t cp, char *out, size_t pos, size_t cap) {
  uint8_t buf[4];
  size_t n;
  if (cp < 0x80) {
    buf[0] = static_cast<uint8_t>(cp);
    n = 1;
  } else if (cp < 0x800) {
    buf[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    buf[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    buf[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    buf[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    buf[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
    buf[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    n = 4;
  }
  if (pos + n > cap)
    return pos;
  memcpy(out + pos, buf, n);
  return pos + n;
}

size_t strip_name_end(char *out, size_t len) {
  while (len > 0 && (out[len - 1] == '.' || out[len - 1] == ' '))
    len--;
  return len;
}

size_t iso_name(const uint8_t *src, size_t len, char *out, size_t cap) {
  size_t pos = 0;
  for (size_t i = 0; i < len && src[i] != ';'; i++) {
    const size_t next = put_utf8(src[i], out, pos, cap);
    if (next == pos)
      break;
    pos = next;
  }
  return strip_name_end(out, pos);
}

size_t joliet_name(const uint8_t *src, size_t len, char *out, size_t cap) {
  size_t pos = 0;
  for (size_t i = 0; i + 1 < len; i += 2) {
    uint32_t cp = (static_cast<uint32_t>(src[i]) << 8) | src[i + 1];
    if (cp == ';')
      break;
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
      const uint32_t lo = (static_cast<uint32_t>(src[i + 2]) << 8) | src[i + 3];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp == 0)
      break;
    const size_t next = put_utf8(cp, out, pos, cap);
    if (next == pos)
      break;
    pos = next;
  }
  return strip_name_end(out, pos);
}

bool ascii_equal_nocase(const char *a, const char *b, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char x = a[i];
    char y = b[i];
    if (x >= 'a' && x <= 'z')
      x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z')
      y = static_cast<char>(y - 'a' + 'A');
    if (x != y)
      return false;
  }
  return true;
}

}

void Index::clear() {
  psram_release(this->dirs_);
  psram_release(this->blob_);
  this->dirs_ = nullptr;
  this->blob_ = nullptr;
  this->dir_count_ = 0;
  this->dir_cap_ = 0;
  this->blob_len_ = 0;
  this->blob_cap_ = 0;
  this->volume_bytes_ = 0;
  this->label_[0] = '\0';
}

bool Index::add_dir_(uint32_t lba, uint32_t size, uint32_t *index_out) {
  if (this->dir_count_ == this->dir_cap_) {
    const uint32_t cap = this->dir_cap_ == 0 ? DIRS_START : this->dir_cap_ * 2;
    if (this->blob_cap_ + static_cast<size_t>(cap) * sizeof(DirRec) > INDEX_MAX)
      return false;
    auto *grown = psram_grow(this->dirs_, cap);
    if (grown == nullptr)
      return false;
    this->dirs_ = grown;
    this->dir_cap_ = cap;
  }
  DirRec &d = this->dirs_[this->dir_count_];
  d.lba = lba;
  d.size = size;
  d.first = 0;
  d.count = 0;
  *index_out = this->dir_count_++;
  return true;
}

uint8_t *Index::add_entry_(size_t name_len) {
  const size_t need = REC_HEAD + name_len;
  if (this->blob_len_ + need > this->blob_cap_) {
    size_t cap = this->blob_cap_ == 0 ? BLOB_START : this->blob_cap_ * 2;
    while (cap < this->blob_len_ + need)
      cap *= 2;
    const size_t dirs_bytes = static_cast<size_t>(this->dir_cap_) * sizeof(DirRec);
    if (cap + dirs_bytes > INDEX_MAX)
      cap = INDEX_MAX - dirs_bytes;
    if (this->blob_len_ + need > cap)
      return nullptr;
    auto *grown = psram_grow(this->blob_, cap);
    if (grown == nullptr)
      return nullptr;
    this->blob_ = grown;
    this->blob_cap_ = cap;
  }
  uint8_t *rec = this->blob_ + this->blob_len_;
  this->blob_len_ += need;
  return rec;
}

void Index::trim_() {
  if (this->blob_len_ > 0 && this->blob_len_ < this->blob_cap_) {
    auto *t = psram_grow(this->blob_, this->blob_len_);
    if (t != nullptr) {
      this->blob_ = t;
      this->blob_cap_ = this->blob_len_;
    }
  }
  if (this->dir_count_ > 0 && this->dir_count_ < this->dir_cap_) {
    auto *t = psram_grow(this->dirs_, this->dir_count_);
    if (t != nullptr) {
      this->dirs_ = t;
      this->dir_cap_ = this->dir_count_;
    }
  }
}

void Index::read_entry_(const uint8_t *rec, Entry *out, const char **name, uint8_t *name_len) {
  uint64_t size;
  memcpy(&out->lba, rec, 4);
  memcpy(&size, rec + 4, 8);
  memcpy(&out->mtime, rec + 12, 4);
  out->size = size;
  out->is_dir = (rec[16] & FLAG_DIR) != 0;
  out->contiguous = (rec[16] & FLAG_FRAGMENTED) == 0;
  if (name != nullptr)
    *name = reinterpret_cast<const char *>(rec + REC_HEAD);
  if (name_len != nullptr)
    *name_len = rec[17];
}

bool Index::find_in_dir_(uint32_t dir, const char *name, size_t name_len, const uint8_t **rec_out) const {
  if (dir >= this->dir_count_)
    return false;
  const DirRec &d = this->dirs_[dir];
  size_t off = d.first;
  for (uint32_t i = 0; i < d.count; i++) {
    const uint8_t *rec = this->blob_ + off;
    const uint8_t nl = rec[17];
    if (nl == name_len && ascii_equal_nocase(reinterpret_cast<const char *>(rec + REC_HEAD), name, name_len)) {
      *rec_out = rec;
      return true;
    }
    off += REC_HEAD + nl;
  }
  return false;
}

StorageError Index::lookup(const char *path, Entry *out) const {
  if (path == nullptr || out == nullptr)
    return StorageError::STORAGE_ERROR_INVALID_ARGS;
  if (this->dir_count_ == 0)
    return StorageError::STORAGE_ERROR_NOT_READY;
  Entry cur{};
  cur.is_dir = true;
  const char *p = path;
  while (*p != '\0') {
    while (*p == '/')
      p++;
    if (*p == '\0')
      break;
    const char *end = p;
    while (*end != '\0' && *end != '/')
      end++;
    if (!cur.is_dir)
      return StorageError::STORAGE_ERROR_NOT_FOUND;
    const uint8_t *rec = nullptr;
    if (!this->find_in_dir_(cur.lba, p, static_cast<size_t>(end - p), &rec))
      return StorageError::STORAGE_ERROR_NOT_FOUND;
    read_entry_(rec, &cur, nullptr, nullptr);
    p = end;
  }
  *out = cur;
  return StorageError::STORAGE_ERROR_OK;
}

StorageError Index::stat(const char *path, FileStat *st) const {
  Entry e{};
  StorageError err = this->lookup(path, &e);
  if (err != StorageError::STORAGE_ERROR_OK)
    return err;
  const char *base = path;
  for (const char *p = path; *p != '\0'; p++) {
    if (*p == '/' && p[1] != '\0' && p[1] != '/')
      base = p + 1;
  }
  size_t n = 0;
  while (base[n] != '\0' && base[n] != '/' && n < STORAGE_NAME_MAX)
    n++;
  memcpy(st->name, base, n);
  st->name[n] = '\0';
  st->is_dir = e.is_dir;
  st->size = e.is_dir ? 0 : e.size;
  st->mtime = e.mtime;
  return StorageError::STORAGE_ERROR_OK;
}

StorageError Index::list(const char *path, bool (*callback)(const FileStat *entry, void *ctx), void *ctx) const {
  Entry dir{};
  StorageError err = this->lookup(path, &dir);
  if (err != StorageError::STORAGE_ERROR_OK)
    return err;
  if (!dir.is_dir)
    return StorageError::STORAGE_ERROR_INVALID_ARGS;
  const DirRec &d = this->dirs_[dir.lba];
  size_t off = d.first;
  FileStat st{};
  for (uint32_t i = 0; i < d.count; i++) {
    const uint8_t *rec = this->blob_ + off;
    Entry e{};
    const char *name = nullptr;
    uint8_t nl = 0;
    read_entry_(rec, &e, &name, &nl);
    off += REC_HEAD + nl;
    memcpy(st.name, name, nl);
    st.name[nl] = '\0';
    st.is_dir = e.is_dir;
    st.size = e.is_dir ? 0 : e.size;
    st.mtime = e.mtime;
    if (!callback(&st, ctx))
      break;
  }
  return StorageError::STORAGE_ERROR_OK;
}

void Builder::start(Index *index, uint32_t session_lba, uint32_t max_sectors) {
  this->index_ = index;
  index->clear();
  this->phase_ = Phase::DESCRIPTORS;
  this->error_ = StorageError::STORAGE_ERROR_OK;
  this->session_lba_ = session_lba;
  this->max_sectors_ = max_sectors == 0 ? 1 : max_sectors;
  this->vd_next_ = 0;
  this->requested_lba_ = 0;
  this->requested_count_ = 0;
  this->root_lba_ = 0;
  this->root_size_ = 0;
  this->joliet_ = false;
  this->have_primary_ = false;
  this->cur_dir_ = 0;
  this->cur_done_ = 0;
  this->pending_multi_ = -1;
}

void Builder::fail_(StorageError err) {
  if (this->phase_ == Phase::FAILED)
    return;
  this->phase_ = Phase::FAILED;
  this->error_ = err;
  this->index_->clear();
}

Builder::Step Builder::next(uint32_t *lba, uint32_t *count) {
  for (;;) {
    switch (this->phase_) {
      case Phase::DESCRIPTORS:
        this->requested_lba_ = this->session_lba_ + 16 + this->vd_next_;
        this->requested_count_ = 1;
        *lba = this->requested_lba_;
        *count = 1;
        return Step::READ;
      case Phase::DIRECTORIES: {
        if (this->cur_dir_ >= this->index_->dir_count_) {
          this->index_->trim_();
          this->phase_ = Phase::DONE;
          return Step::DONE;
        }
        const Index::DirRec &d = this->index_->dirs_[this->cur_dir_];
        if (this->cur_done_ >= d.size) {
          this->cur_dir_++;
          this->cur_done_ = 0;
          this->pending_multi_ = -1;
          continue;
        }
        const uint32_t remaining = (d.size - this->cur_done_ + SECTOR_SIZE - 1) / SECTOR_SIZE;
        this->requested_lba_ = d.lba + this->cur_done_ / SECTOR_SIZE;
        this->requested_count_ = remaining < this->max_sectors_ ? remaining : this->max_sectors_;
        *lba = this->requested_lba_;
        *count = this->requested_count_;
        return Step::READ;
      }
      case Phase::DONE:
        return Step::DONE;
      case Phase::FAILED:
      default:
        return Step::FAILED;
    }
  }
}

void Builder::take_descriptor_(const uint8_t *vd) {
  const uint8_t type = vd[0];
  const bool joliet = type == 2 && vd[88] == 0x25 && vd[89] == 0x2F && (vd[90] == 0x40 || vd[90] == 0x43 || vd[90] == 0x45);
  if (type != 1 && !joliet)
    return;
  if (le16(vd + 128) != SECTOR_SIZE) {
    if (type == 1)
      this->fail_(StorageError::STORAGE_ERROR_NOT_SUPPORTED);
    return;
  }
  if (type == 1 && this->joliet_) {
    this->have_primary_ = true;
    return;
  }
  const uint8_t *root = vd + 156;
  this->root_lba_ = le32(root + 2) + root[1];
  this->root_size_ = le32(root + 10);
  this->index_->volume_bytes_ = static_cast<uint64_t>(le32(vd + 80)) * SECTOR_SIZE;
  size_t n = joliet ? joliet_name(vd + 40, 32, this->index_->label_, LABEL_SIZE - 1)
                    : iso_name(vd + 40, 32, this->index_->label_, LABEL_SIZE - 1);
  this->index_->label_[n] = '\0';
  if (type == 1) {
    this->have_primary_ = true;
  } else {
    this->joliet_ = true;
  }
}

void Builder::begin_directories_() {
  if (!this->have_primary_ && !this->joliet_) {
    this->fail_(StorageError::STORAGE_ERROR_CORRUPT);
    return;
  }
  if (this->root_size_ == 0 || this->root_size_ > DIR_SIZE_MAX) {
    this->fail_(StorageError::STORAGE_ERROR_CORRUPT);
    return;
  }
  uint32_t root = 0;
  if (!this->index_->add_dir_(this->root_lba_, this->root_size_, &root)) {
    this->fail_(StorageError::STORAGE_ERROR_NO_SPACE);
    return;
  }
  this->phase_ = Phase::DIRECTORIES;
  this->cur_dir_ = 0;
  this->cur_done_ = 0;
  this->pending_multi_ = -1;
}

void Builder::add_record_(const uint8_t *rec) {
  const uint8_t len = rec[0];
  const uint8_t nl = rec[32];
  if (33u + nl > len) {
    this->fail_(StorageError::STORAGE_ERROR_CORRUPT);
    return;
  }
  const uint8_t flags = rec[25];
  if ((flags & 0x04) != 0)
    return;
  if (nl == 1 && (rec[33] == 0 || rec[33] == 1))
    return;
  char name[STORAGE_NAME_MAX];
  const size_t n = this->joliet_ ? joliet_name(rec + 33, nl, name, sizeof(name)) : iso_name(rec + 33, nl, name, sizeof(name));
  if (n == 0)
    return;
  const bool is_dir = (flags & 0x02) != 0;
  const uint32_t lba = le32(rec + 2) + rec[1];
  const uint32_t size = le32(rec + 10);
  const bool interleaved = rec[26] != 0 || rec[27] != 0;
  Index &ix = *this->index_;

  if (this->pending_multi_ >= 0) {
    uint8_t *prev = ix.blob_ + this->pending_multi_;
    if (!is_dir && prev[17] == n && memcmp(prev + REC_HEAD, name, n) == 0) {
      uint32_t plba;
      uint64_t psize;
      memcpy(&plba, prev, 4);
      memcpy(&psize, prev + 4, 8);
      if (psize % SECTOR_SIZE != 0 || static_cast<uint64_t>(lba) != plba + psize / SECTOR_SIZE || interleaved)
        prev[16] |= FLAG_FRAGMENTED;
      psize += size;
      memcpy(prev + 4, &psize, 8);
      if ((flags & 0x80) == 0) {
        prev[16] &= static_cast<uint8_t>(~FLAG_OPEN_MULTI);
        this->pending_multi_ = -1;
      }
      return;
    }
    this->pending_multi_ = -1;
  }

  uint32_t stored = lba;
  if (is_dir) {
    if (size > DIR_SIZE_MAX) {
      this->fail_(StorageError::STORAGE_ERROR_CORRUPT);
      return;
    }
    if (!ix.add_dir_(lba, size, &stored)) {
      this->fail_(StorageError::STORAGE_ERROR_NO_SPACE);
      return;
    }
  }
  uint8_t *out = ix.add_entry_(n);
  if (out == nullptr) {
    this->fail_(StorageError::STORAGE_ERROR_NO_SPACE);
    return;
  }
  const uint64_t size64 = is_dir ? 0 : size;
  const uint32_t mtime = record_time(rec + 18);
  memcpy(out, &stored, 4);
  memcpy(out + 4, &size64, 8);
  memcpy(out + 12, &mtime, 4);
  out[16] = static_cast<uint8_t>((is_dir ? FLAG_DIR : 0) | (interleaved ? FLAG_FRAGMENTED : 0));
  out[17] = static_cast<uint8_t>(n);
  memcpy(out + REC_HEAD, name, n);
  ix.dirs_[this->cur_dir_].count++;
  if (!is_dir && (flags & 0x80) != 0) {
    out[16] |= FLAG_OPEN_MULTI;
    this->pending_multi_ = static_cast<int64_t>(out - ix.blob_);
  }
}

void Builder::parse_dir_sector_(const uint8_t *sector, uint32_t valid) {
  uint32_t pos = 0;
  while (pos < valid && this->phase_ == Phase::DIRECTORIES) {
    const uint8_t len = sector[pos];
    if (len == 0)
      break;
    if (len < 34 || pos + len > valid) {
      this->fail_(StorageError::STORAGE_ERROR_CORRUPT);
      return;
    }
    this->add_record_(sector + pos);
    pos += len;
  }
}

void Builder::feed(const uint8_t *data, uint32_t count) {
  if (count != this->requested_count_) {
    this->fail_(StorageError::STORAGE_ERROR_READ_ERROR);
    return;
  }
  if (this->phase_ == Phase::DESCRIPTORS) {
    if (memcmp(data + 1, "CD001", 5) != 0 || data[0] == 255) {
      this->begin_directories_();
      return;
    }
    this->take_descriptor_(data);
    if (this->phase_ == Phase::FAILED)
      return;
    if (++this->vd_next_ >= MAX_DESCRIPTORS)
      this->begin_directories_();
    return;
  }
  if (this->phase_ != Phase::DIRECTORIES)
    return;
  if (this->cur_done_ == 0) {
    this->index_->dirs_[this->cur_dir_].first = static_cast<uint32_t>(this->index_->blob_len_);
    this->index_->dirs_[this->cur_dir_].count = 0;
  }
  for (uint32_t i = 0; i < count && this->phase_ == Phase::DIRECTORIES; i++) {
    const uint32_t size = this->index_->dirs_[this->cur_dir_].size;
    const uint32_t left = size - this->cur_done_;
    this->parse_dir_sector_(data + static_cast<size_t>(i) * SECTOR_SIZE, left < SECTOR_SIZE ? left : SECTOR_SIZE);
    this->cur_done_ = left < SECTOR_SIZE ? size : this->cur_done_ + SECTOR_SIZE;
  }
}

}

#endif
