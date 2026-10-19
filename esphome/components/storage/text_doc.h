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
// PSRAM), EDIT_MAX + 1 bytes, allocated on first use and kept; the undo step (UNDO_MAX) and the view
// buffer (VIEW_MAX, filtered log text) the same way when first used. Only Doc's few fields are internal.

#include <algorithm>
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
static constexpr size_t UNDO_MAX = 4 * 1024;     // the text one undo step can bring back
static constexpr size_t VIEW_MAX = 64 * 1024;    // the filtered text a log view shows
static constexpr uint32_t UNDO_MERGE_MS = 2000;  // typing within this joins the same undo step

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
  bool dirty{false};         // changed since it was read or saved (track())
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
  if (err == st::StorageError::STORAGE_ERROR_OK)
    d.dirty = false;
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

// ---- Editing: the buffer follows the text area; one undo step ----------------------------------------

struct Undo {
  size_t pos;          // where the change happened
  size_t inserted;     // bytes the change put in at pos
  size_t removed_len;  // bytes it took out (kept in removed)
  uint32_t at_ms;      // when it was last extended
  bool valid;
  char removed[UNDO_MAX];
};

inline Undo *undo_() {
  static Undo *u = nullptr;
  if (u == nullptr) {
    esphome::RAMAllocator<Undo> alloc(esphome::RAMAllocator<Undo>::ALLOC_EXTERNAL);
    u = alloc.allocate(1);
    if (u != nullptr)
      u->valid = false;
  }
  return u;
}

// The text area's text now (`now`, n bytes): the buffer takes it, the difference becomes the undo step
// (joined with the step before while typing goes on at its end, or backspacing / deleting at its start,
// within UNDO_MERGE_MS). A change larger than UNDO_MAX cannot be undone.
inline void track(const char *now, size_t n, uint32_t now_ms) {
  Doc &d = doc();
  if (d.buf == nullptr || now == nullptr || n > EDIT_MAX)
    return;
  size_t pre = 0;
  const size_t shorter = std::min(d.len, n);
  while (pre < shorter && d.buf[pre] == now[pre])
    pre++;
  size_t suf = 0;
  while (suf < shorter - pre && d.buf[d.len - 1 - suf] == now[n - 1 - suf])
    suf++;
  const size_t removed = d.len - pre - suf;
  const size_t inserted = n - pre - suf;
  if (removed == 0 && inserted == 0)
    return;
  Undo *u = undo_();
  if (u != nullptr) {
    const bool recent = u->valid && now_ms - u->at_ms < UNDO_MERGE_MS;
    if (recent && removed == 0 && pre == u->pos + u->inserted) {
      u->inserted += inserted;  // typing on
    } else if (recent && inserted == 0 && u->inserted == 0 && pre + removed == u->pos &&
               u->removed_len + removed <= UNDO_MAX) {
      memmove(u->removed + removed, u->removed, u->removed_len);  // backspacing on
      memcpy(u->removed, d.buf + pre, removed);
      u->removed_len += removed;
      u->pos = pre;
    } else if (recent && inserted == 0 && u->inserted == 0 && pre == u->pos &&
               u->removed_len + removed <= UNDO_MAX) {
      memcpy(u->removed + u->removed_len, d.buf + pre, removed);  // deleting forward on
      u->removed_len += removed;
    } else if (removed <= UNDO_MAX) {
      u->pos = pre;
      u->inserted = inserted;
      u->removed_len = removed;
      memcpy(u->removed, d.buf + pre, removed);
      u->valid = true;
    } else {
      u->valid = false;
    }
    u->at_ms = now_ms;
  }
  memmove(d.buf + pre + inserted, d.buf + d.len - suf, suf);
  memcpy(d.buf + pre, now + pre, inserted);
  d.len = n;
  d.buf[n] = '\0';
  d.dirty = true;
}

inline bool can_undo() {
  Undo *u = undo_();
  return u != nullptr && u->valid;
}

// Takes the last step back in the buffer; the caller shows the buffer again. Returns where the cursor
// goes (the end of what came back), or -1 when there is nothing to undo.
inline int64_t undo() {
  Doc &d = doc();
  Undo *u = undo_();
  if (u == nullptr || !u->valid || d.buf == nullptr || u->pos + u->inserted > d.len ||
      d.len - u->inserted + u->removed_len > EDIT_MAX)
    return -1;
  const size_t tail = d.len - u->pos - u->inserted;
  memmove(d.buf + u->pos + u->removed_len, d.buf + u->pos + u->inserted, tail);
  memcpy(d.buf + u->pos, u->removed, u->removed_len);
  d.len = d.len - u->inserted + u->removed_len;
  d.buf[d.len] = '\0';
  u->valid = false;
  d.dirty = true;
  return static_cast<int64_t>(u->pos + u->removed_len);
}

inline void forget_undo() {
  Undo *u = undo_();
  if (u != nullptr)
    u->valid = false;
}

// ---- Search and replace (in the buffer; case-insensitive for ASCII) ----------------------------------

inline bool match_(const char *p, const char *needle, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char a = p[i], b = needle[i];
    if (a >= 'A' && a <= 'Z')
      a = static_cast<char>(a + 32);
    if (b >= 'A' && b <= 'Z')
      b = static_cast<char>(b + 32);
    if (a != b)
      return false;
  }
  return true;
}

// The next match at or after `from` (forward) or before `from` (backward), wrapping round; -1 none.
inline int64_t find(const char *needle, size_t from, bool forward) {
  const Doc &d = doc();
  const size_t n = needle == nullptr ? 0 : strlen(needle);
  if (d.buf == nullptr || n == 0 || n > d.len)
    return -1;
  const size_t count = d.len - n + 1;  // positions a match can start at
  from = std::min(from, count);
  for (size_t k = 0; k < count; k++) {
    const size_t i = forward ? (from + k) % count : (from + 2 * count - 1 - k) % count;
    if (match_(d.buf + i, needle, n))
      return static_cast<int64_t>(i);
  }
  return -1;
}

// Replaces the match at `pos` (it must match `needle`); false when it does not fit
inline bool replace_at(size_t pos, const char *needle, const char *with) {
  Doc &d = doc();
  const size_t n = strlen(needle), m = strlen(with);
  if (d.buf == nullptr || n == 0 || pos + n > d.len || !match_(d.buf + pos, needle, n) || d.len - n + m > EDIT_MAX)
    return false;
  memmove(d.buf + pos + m, d.buf + pos + n, d.len - pos - n);
  memcpy(d.buf + pos, with, m);
  d.len = d.len - n + m;
  d.buf[d.len] = '\0';
  d.dirty = true;
  forget_undo();
  return true;
}

// Every match replaced; returns how many
inline size_t replace_all(const char *needle, const char *with) {
  const size_t n = needle == nullptr ? 0 : strlen(needle), m = strlen(with);
  if (n == 0)
    return 0;
  size_t count = 0, pos = 0;
  Doc &d = doc();
  while (pos + n <= d.len) {
    if (match_(d.buf + pos, needle, n)) {
      if (!replace_at(pos, needle, with))
        break;
      count++;
      pos += m;
    } else {
      pos++;
    }
  }
  return count;
}

// Line and column (1-based) of byte `pos`
inline void line_col(size_t pos, size_t *line, size_t *col) {
  const Doc &d = doc();
  size_t l = 1, c = 1;
  for (size_t i = 0; d.buf != nullptr && i < pos && i < d.len; i++) {
    if (d.buf[i] == '\n') {
      l++;
      c = 1;
    } else {
      c++;
    }
  }
  *line = l;
  *col = c;
}

// ---- Log text: lines filtered by level into the view buffer -------------------------------------------

inline char *view_buffer() {
  static char *v = nullptr;
  if (v == nullptr) {
    esphome::RAMAllocator<char> alloc(esphome::RAMAllocator<char>::ALLOC_EXTERNAL);
    v = alloc.allocate(VIEW_MAX + 1);
    if (v != nullptr)
      v[0] = '\0';
  }
  return v;
}

// An ESPHome log line's level: the letter in its first "[X]" (E W I C D V -> 1..6); 0 none (always shown)
inline uint8_t line_level(const char *p, size_t n) {
  for (size_t i = 0; i + 2 < n && i < 40; i++) {
    if (p[i] == '[' && p[i + 2] == ']') {
      switch (p[i + 1]) {
        case 'E':
          return 1;
        case 'W':
          return 2;
        case 'I':
          return 3;
        case 'C':
          return 4;
        case 'D':
          return 5;
        case 'V':
          return 6;
        default:
          return 0;
      }
    }
  }
  return 0;
}

// The document's lines with a level up to `max_level` (1 E .. 6 V; lines without one always) into the
// view buffer, the newest that fit; returns the view (NUL terminated) or nullptr without PSRAM.
inline const char *filter_levels(uint8_t max_level) {
  const Doc &d = doc();
  char *v = view_buffer();
  if (v == nullptr || d.buf == nullptr)
    return nullptr;
  size_t out = 0;
  size_t i = 0;
  while (i < d.len) {
    size_t end = i;
    while (end < d.len && d.buf[end] != '\n')
      end++;
    const size_t line = end - i + (end < d.len ? 1 : 0);
    if (line_level(d.buf + i, end - i) <= max_level) {
      if (out + line > VIEW_MAX) {  // keep the newest: drop the oldest half
        const size_t drop = std::min(out, std::max(out / 2, out + line - VIEW_MAX));
        memmove(v, v + drop, out - drop);
        out -= drop;
      }
      const size_t take = std::min(line, VIEW_MAX - out);
      memcpy(v + out, d.buf + i, take);
      out += take;
    }
    i = end + 1;
  }
  v[out] = '\0';
  return v;
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
