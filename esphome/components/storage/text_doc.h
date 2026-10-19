#pragma once

// WARNING: This component is EXPERIMENTAL. The API may change at any time
// without following the normal breaking changes policy. Use at your own risk.

// A text document model for an editor / log viewer UI on top of the storage interface and its worker:
// one file at a time, read through a worker stream into one PSRAM buffer; a file up to EDIT_MAX whole
// (editable), a larger one as a read-only window of WINDOW_MAX bytes at an offset (paged); a log file
// followed while it grows (the bytes past what was read are appended, the oldest dropped); saved as
// <name>.tmp and renamed over the file only once it is written whole (a failed save never leaves half a
// file). Completions on the main loop. No UI here: a consumer (e.g. an LVGL page) shows doc().
//
// RAM: the buffer is PSRAM only (ALLOC_EXTERNAL, no internal fallback; ensure_memory() is false without
// PSRAM), EDIT_MAX + 1 bytes, allocated on first use and kept. Only Doc's few fields are internal.

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "esphome/core/helpers.h"
#include "storage.h"
#ifdef USE_STORAGE_WORKER
#include "storage_worker.h"
#endif

namespace esphome::storage::text {

namespace st = esphome::storage;

static constexpr size_t EDIT_MAX = 256 * 1024;   // a file up to this is read whole and editable
static constexpr size_t WINDOW_MAX = 32 * 1024;  // a larger one: a read-only window this big
static constexpr size_t CHUNK = 16 * 1024;       // per read_chunk / write_chunk

using Done = std::function<void(st::StorageError)>;

struct Doc {
  char *buf{nullptr};  // PSRAM, EDIT_MAX + 1 (always NUL terminated at len)
  size_t len{0};
  std::string path;          // the full VFS path of the file shown
  uint64_t file_size{0};     // as last read
  uint64_t offset{0};        // where buf starts in the file (0 unless a window)
  bool editable{false};      // read whole, no NUL bytes
  bool binary{false};        // NUL bytes found (shown as '.')
  bool busy{false};          // a load / follow / save runs
  st::StorageError last{st::StorageError::STORAGE_ERROR_OK};
#ifdef USE_STORAGE_WORKER
  // The running operation
  st::StreamHandle stream{};
  st::PathStorage *ps{nullptr};
  std::string rel;
  std::string tmp_rel;
  size_t want{0};
  size_t got{0};
  size_t step{0};
  bool append{false};
  Done done;
#endif
};

inline Doc &doc() {
  static Doc d;
  return d;
}

inline bool ensure_memory() {
  Doc &d = doc();
  if (d.buf == nullptr) {
    esphome::RAMAllocator<char> alloc(esphome::RAMAllocator<char>::ALLOC_EXTERNAL);
    d.buf = alloc.allocate(EDIT_MAX + 1);
    if (d.buf != nullptr)
      d.buf[0] = '\0';
  }
  return d.buf != nullptr;
}

inline st::PathStorage *resolve_(const std::string &path, std::string *rel) {
  if (st::global_storage_registry == nullptr)
    return nullptr;
  const char *r = nullptr;
  st::PathStorage *ps = st::global_storage_registry->resolve_path(path.c_str(), &r);
  if (ps != nullptr && rel != nullptr)
    *rel = (r == nullptr || r[0] == '\0') ? std::string("/") : std::string(r);
  return ps;
}

inline bool task_busy_(const st::Storage *s) {
#ifdef USE_STORAGE_WORKER
  return st::global_storage_worker != nullptr && st::global_storage_worker->has_active_task_io(s);
#else
  return false;
#endif
}

// NUL bytes would end LVGL's text: shown as '.', the document then read only
inline void clean_(char *p, size_t n, bool &binary) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] == '\0') {
      p[i] = '.';
      binary = true;
    }
  }
}

inline void finish_(st::StorageError err) {
  Doc &d = doc();
  d.busy = false;
  d.last = err;
  d.buf[d.len] = '\0';
  Done done = std::move(d.done);
  d.done = nullptr;
  if (done)
    done(err);
}

#ifdef USE_STORAGE_WORKER
inline void read_step_();

inline void read_close_(st::StorageError err) {
  Doc &d = doc();
  st::global_storage_worker->end_read(d.stream, [err](st::StorageError) { finish_(err); });
}

inline void read_step_() {
  Doc &d = doc();
  if (d.got >= d.want) {
    read_close_(st::StorageError::STORAGE_ERROR_OK);
    return;
  }
  const size_t n = std::min(CHUNK, d.want - d.got);
  // A follow that outgrows the buffer drops the oldest half first
  if (d.len + n > EDIT_MAX) {
    const size_t drop = std::max(d.len / 2, d.len + n - EDIT_MAX);
    memmove(d.buf, d.buf + drop, d.len - drop);
    d.len -= drop;
    d.offset += drop;
    d.editable = false;
  }
  d.step = 0;
  const st::StorageError err = st::global_storage_worker->read_chunk(
      d.stream, reinterpret_cast<uint8_t *>(d.buf + d.len), n, &d.step, [](st::StorageError e) {
        Doc &d = doc();
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          read_close_(e);
          return;
        }
        clean_(d.buf + d.len, d.step, d.binary);
        d.len += d.step;
        d.got += d.step;
        if (d.step == 0) {  // EOF before what stat said: the file shrank
          read_close_(st::StorageError::STORAGE_ERROR_OK);
          return;
        }
        read_step_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK)
    read_close_(err);
}

// Opens d.rel on d.ps, seeks to `from`, reads d.want bytes into the buffer (after d.len)
inline st::StorageError read_range_(uint64_t from) {
  Doc &d = doc();
  const st::StorageError err =
      st::global_storage_worker->begin_read(d.ps, d.rel.c_str(), &d.stream, [from](st::StorageError e) {
        Doc &d = doc();
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          finish_(e);
          return;
        }
        if (from == 0) {
          read_step_();
          return;
        }
        const st::StorageError s = st::global_storage_worker->seek(
            d.stream, static_cast<int64_t>(from), st::SeekMode::SEEK_MODE_SET, [](st::StorageError e2) {
              if (e2 != st::StorageError::STORAGE_ERROR_OK) {
                read_close_(e2);
                return;
              }
              read_step_();
            });
        if (s != st::StorageError::STORAGE_ERROR_OK)
          read_close_(s);
      });
  return err;
}
#endif

// The file's size and kind now (blocking stat, guarded like the actions)
inline st::StorageError stat_(const std::string &path, st::PathStorage **ps, std::string *rel, st::FileStat *info) {
  *ps = resolve_(path, rel);
  if (*ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy_(*ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  const st::StorageError err = (*ps)->stat(rel->c_str(), info);
  if (err == st::StorageError::STORAGE_ERROR_OK && info->is_dir)
    return st::StorageError::STORAGE_ERROR_INVALID_ARGS;
  return err;
}

// Loads `path`: whole when it is up to EDIT_MAX, else the window at `from` (-1: its end). `done` fires
// once on the main loop unless an immediate error is returned.
inline st::StorageError load(const std::string &path, int64_t from, Done done) {
#ifdef USE_STORAGE_WORKER
  Doc &d = doc();
  if (d.busy)
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  if (!ensure_memory() || st::global_storage_worker == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  st::FileStat info{};
  st::PathStorage *ps = nullptr;
  std::string rel;
  const st::StorageError err = stat_(path, &ps, &rel, &info);
  if (err != st::StorageError::STORAGE_ERROR_OK)
    return err;
  d.path = path;
  d.ps = ps;
  d.rel = rel;
  d.file_size = info.size;
  d.binary = false;
  d.len = 0;
  d.got = 0;
  d.buf[0] = '\0';
  uint64_t start = 0;
  if (info.size <= EDIT_MAX) {
    d.want = static_cast<size_t>(info.size);
    d.editable = true;
  } else {
    const uint64_t last = info.size - WINDOW_MAX;
    start = from < 0 ? last : std::min<uint64_t>(static_cast<uint64_t>(from), last);
    d.want = WINDOW_MAX;
    d.editable = false;
  }
  d.offset = start;
  d.done = [done = std::move(done)](st::StorageError e) {
    Doc &d = doc();
    if (d.binary)
      d.editable = false;
    if (done)
      done(e);
  };
  d.busy = true;
  const st::StorageError r = read_range_(start);
  if (r != st::StorageError::STORAGE_ERROR_OK) {
    d.busy = false;
    d.done = nullptr;
  }
  return r;
#else
  return st::StorageError::STORAGE_ERROR_NOT_READY;
#endif
}

// The bytes the file got since it was read (a log that grows) appended; the oldest dropped when the
// buffer is full (the document then read only). OK with nothing new: done(OK) at once is not called -
// the return is ALREADY_EXISTS ("nothing to read").
inline st::StorageError follow(Done done) {
#ifdef USE_STORAGE_WORKER
  Doc &d = doc();
  if (d.busy || d.buf == nullptr || d.path.empty())
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  st::FileStat info{};
  st::PathStorage *ps = nullptr;
  std::string rel;
  const st::StorageError err = stat_(d.path, &ps, &rel, &info);
  if (err != st::StorageError::STORAGE_ERROR_OK)
    return err;
  const uint64_t end = d.offset + d.len;
  if (info.size < end)  // truncated or replaced: read it again from the start of its last window
    return load(d.path, -1, std::move(done));
  if (info.size == end)
    return st::StorageError::STORAGE_ERROR_ALREADY_EXISTS;
  d.ps = ps;
  d.rel = rel;
  d.file_size = info.size;
  d.want = static_cast<size_t>(std::min<uint64_t>(info.size - end, EDIT_MAX));
  d.got = 0;
  d.done = std::move(done);
  d.busy = true;
  const st::StorageError r = read_range_(info.size - d.want);
  if (r != st::StorageError::STORAGE_ERROR_OK) {
    d.busy = false;
    d.done = nullptr;
  }
  return r;
#else
  return st::StorageError::STORAGE_ERROR_NOT_READY;
#endif
}

#ifdef USE_STORAGE_WORKER
inline void save_fail_(st::StorageError err) {
  Doc &d = doc();
  st::global_storage_worker->end_write(d.stream, [err](st::StorageError) {
    Doc &d = doc();
    if (!task_busy_(d.ps))
      d.ps->remove(d.tmp_rel.c_str());
    finish_(err);
  });
}

inline void save_step_() {
  Doc &d = doc();
  if (d.got >= d.len) {
    st::global_storage_worker->end_write(d.stream, [](st::StorageError e) {
      Doc &d = doc();
      if (e != st::StorageError::STORAGE_ERROR_OK) {
        finish_(e);
        return;
      }
      // Written whole: the old file goes, the new one takes its name (FAT's rename does not replace)
      if (task_busy_(d.ps)) {
        finish_(st::StorageError::STORAGE_ERROR_NOT_READY);
        return;
      }
      st::FileStat old{};
      if (d.ps->stat(d.rel.c_str(), &old) == st::StorageError::STORAGE_ERROR_OK) {
        const st::StorageError r = d.ps->remove(d.rel.c_str());
        if (r != st::StorageError::STORAGE_ERROR_OK) {
          finish_(r);
          return;
        }
      }
      const st::StorageError r = d.ps->rename(d.tmp_rel.c_str(), d.rel.c_str());
      if (r == st::StorageError::STORAGE_ERROR_OK) {
        d.file_size = d.len;
        d.offset = 0;
#ifdef USE_STORAGE_CHANGE_FEED
        st::global_storage_registry->note_parent_changed(d.path);
#endif
      }
      finish_(r);
    });
    return;
  }
  const size_t n = std::min(CHUNK, d.len - d.got);
  const st::StorageError err = st::global_storage_worker->write_chunk(
      d.stream, reinterpret_cast<const uint8_t *>(d.buf + d.got), n, [n](st::StorageError e) {
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          save_fail_(e);
          return;
        }
        doc().got += n;
        save_step_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK)
    save_fail_(err);
}
#endif

// Saves `text` (len bytes, copied into the document's buffer first) to `path` (a new path: save as).
inline st::StorageError save(const std::string &path, const char *text, size_t len, Done done) {
#ifdef USE_STORAGE_WORKER
  Doc &d = doc();
  if (d.busy)
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  if (!ensure_memory() || st::global_storage_worker == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  if (len > EDIT_MAX)
    return st::StorageError::STORAGE_ERROR_INVALID_ARGS;
  std::string rel;
  st::PathStorage *ps = resolve_(path, &rel);
  if (ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy_(ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  if (text != d.buf)
    memmove(d.buf, text, len);
  d.buf[len] = '\0';
  d.len = len;
  d.path = path;
  d.ps = ps;
  d.rel = rel;
  d.tmp_rel = rel + ".tmp";
  d.got = 0;
  d.editable = true;
  d.binary = false;
  d.done = std::move(done);
  d.busy = true;
  const st::StorageError err =
      st::global_storage_worker->begin_write(ps, d.tmp_rel.c_str(), &d.stream, [](st::StorageError e) {
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          finish_(e);
          return;
        }
        save_step_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    d.busy = false;
    d.done = nullptr;
  }
  return err;
#else
  return st::StorageError::STORAGE_ERROR_NOT_READY;
#endif
}

// Lines in the buffer, and the byte where line n (0-based) starts (for "go to line")
inline size_t line_count() {
  const Doc &d = doc();
  if (d.buf == nullptr || d.len == 0)
    return 0;
  size_t n = 1;
  for (size_t i = 0; i < d.len; i++)
    n += d.buf[i] == '\n';
  return n;
}

inline size_t line_start(size_t line) {
  const Doc &d = doc();
  size_t pos = 0;
  for (size_t n = 0; n < line && pos < d.len; pos++) {
    if (d.buf[pos] == '\n')
      n++;
  }
  return pos;
}

}  // namespace esphome::storage::text
