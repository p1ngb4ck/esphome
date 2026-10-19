#pragma once

// WARNING: This component is EXPERIMENTAL. The API may change at any time
// without following the normal breaking changes policy. Use at your own risk.

// A text file for an editor / log viewer UI on top of the storage interface and its worker - without a
// buffer of its own: the text lives where the UI shows it (an LVGL text area or label keeps its own copy).
// Reading borrows the transfer buffer (global_transfer_buffer, PSRAM) for the read only: the text is
// handed to the caller, which takes it (LVGL copies it), then the buffer goes back. Saving streams the
// caller's text (it stays unchanged until the save is done) to <name>.tmp and renames it over the file
// (a failed save never leaves half a file). A file up to EDIT_MAX is read whole (editable), a larger one
// as its last WINDOW_MAX bytes (read only). Text helpers (search, replace into a borrowed buffer, lines,
// UTF-8 positions, log lines by level) work on any text. Completions on the main loop.
//
// RAM: nothing kept. The transfer buffer is borrowed during a read and while replace_all / a log filter
// builds its result (the caller gives it back); Doc's few fields and the undo record (UNDO_MAX, PSRAM,
// taken on the first edit) stay.

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
#ifdef USE_STORAGE_TRANSFER_BUFFER
#include "transfer_buffer.h"
#endif

namespace esphome::storage::text {

namespace st = esphome::storage;

static constexpr size_t EDIT_MAX = 256 * 1024;   // a file up to this is read whole and editable
static constexpr size_t WINDOW_MAX = 32 * 1024;  // a larger one: its last bytes, read only
static constexpr size_t CHUNK = 16 * 1024;       // per read_chunk / write_chunk
static constexpr size_t UNDO_MAX = 256;          // the text one undo step can bring back
static constexpr uint32_t UNDO_MERGE_MS = 2000;  // typing within this joins the same undo step

using Done = std::function<void(st::StorageError)>;
// The text read (NUL terminated, in the borrowed buffer: take it before returning), or an error
using Read = std::function<void(st::StorageError, const char *text, size_t len)>;

struct Doc {
  std::string path;       // the file shown (full VFS path)
  uint64_t file_size{0};  // as last read or written
  uint64_t offset{0};     // where the text shown starts in the file (0 unless a window)
  bool editable{false};   // read whole, no NUL bytes
  bool binary{false};     // NUL bytes found (shown as '.')
  bool busy{false};       // a read or save runs
  bool dirty{false};      // changed since read or saved (the UI marks it)
#ifdef USE_STORAGE_WORKER
  st::StreamHandle stream{};
  st::PathStorage *ps{nullptr};
  std::string rel;
  std::string tmp_rel;
  uint8_t *buf{nullptr};    // the borrowed transfer buffer while a read runs
  const char *src{nullptr};  // the caller's text while a save runs
  size_t len{0};
  size_t want{0};
  size_t step{0};
  Read read_done;
  Done save_done;
#endif
};

inline Doc &doc() {
  static Doc d;
  return d;
}

// ---- The transfer buffer, borrowed ----------------------------------------------------------------------

inline uint8_t *borrow(size_t need) {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  return st::global_transfer_buffer != nullptr ? st::global_transfer_buffer->try_acquire(need) : nullptr;
#else
  return nullptr;
#endif
}

inline void give_back() {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  if (st::global_transfer_buffer != nullptr)
    st::global_transfer_buffer->release();
#endif
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

#ifdef USE_STORAGE_WORKER
// ---- Reading ---------------------------------------------------------------------------------------------

inline void read_finish_(st::StorageError err) {
  Doc &d = doc();
  d.busy = false;
  char *text = reinterpret_cast<char *>(d.buf);
  if (text != nullptr) {
    text[d.len] = '\0';
    for (size_t i = 0; i < d.len; i++) {  // NUL bytes would end LVGL's text
      if (text[i] == '\0') {
        text[i] = '.';
        d.binary = true;
      }
    }
  }
  if (d.binary)
    d.editable = false;
  if (err == st::StorageError::STORAGE_ERROR_OK)
    d.dirty = false;
  Read done = std::move(d.read_done);
  d.read_done = nullptr;
  if (done)
    done(err, text != nullptr ? text : "", text != nullptr ? d.len : 0);
  d.buf = nullptr;
  give_back();
}

inline void read_close_(st::StorageError err) {
  st::global_storage_worker->end_read(doc().stream, [err](st::StorageError) { read_finish_(err); });
}

inline void read_step_() {
  Doc &d = doc();
  if (d.len >= d.want) {
    read_close_(st::StorageError::STORAGE_ERROR_OK);
    return;
  }
  d.step = 0;
  const st::StorageError err = st::global_storage_worker->read_chunk(
      d.stream, d.buf + d.len, std::min(CHUNK, d.want - d.len), &d.step, [](st::StorageError e) {
        Doc &d = doc();
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          read_close_(e);
          return;
        }
        d.len += d.step;
        if (d.step == 0) {  // EOF before what stat said: the file shrank
          read_close_(st::StorageError::STORAGE_ERROR_OK);
          return;
        }
        read_step_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK)
    read_close_(err);
}
#endif

// Reads `path`: whole when it is up to EDIT_MAX, else its last WINDOW_MAX bytes (read only). `done` gets the
// text once (main loop) unless an immediate error is returned (NOT_READY also when the transfer buffer is
// in use).
inline st::StorageError load(const std::string &path, Read done) {
#ifdef USE_STORAGE_WORKER
  Doc &d = doc();
  if (d.busy || st::global_storage_worker == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  std::string rel;
  st::PathStorage *ps = resolve_(path, &rel);
  if (ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy_(ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  st::FileStat info{};
  st::StorageError err = ps->stat(rel.c_str(), &info);
  if (err != st::StorageError::STORAGE_ERROR_OK)
    return err;
  if (info.is_dir)
    return st::StorageError::STORAGE_ERROR_INVALID_ARGS;
  const bool whole = info.size <= EDIT_MAX;
  const size_t want = whole ? static_cast<size_t>(info.size) : WINDOW_MAX;
  uint8_t *buf = borrow(want + 1);
  if (buf == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  d.path = path;
  d.ps = ps;
  d.rel = rel;
  d.file_size = info.size;
  d.offset = whole ? 0 : info.size - WINDOW_MAX;
  d.editable = whole;
  d.binary = false;
  d.buf = buf;
  d.len = 0;
  d.want = want;
  d.read_done = std::move(done);
  d.busy = true;
  const uint64_t from = d.offset;
  err = st::global_storage_worker->begin_read(ps, rel.c_str(), &d.stream, [from](st::StorageError e) {
    if (e != st::StorageError::STORAGE_ERROR_OK) {
      read_finish_(e);
      return;
    }
    if (from == 0) {
      read_step_();
      return;
    }
    const st::StorageError s = st::global_storage_worker->seek(doc().stream, static_cast<int64_t>(from),
                                                              st::SeekMode::SEEK_MODE_SET, [](st::StorageError e2) {
                                                                if (e2 != st::StorageError::STORAGE_ERROR_OK) {
                                                                  read_close_(e2);
                                                                  return;
                                                                }
                                                                read_step_();
                                                              });
    if (s != st::StorageError::STORAGE_ERROR_OK)
      read_close_(s);
  });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    d.busy = false;
    d.read_done = nullptr;
    d.buf = nullptr;
    give_back();
  }
  return err;
#else
  return st::StorageError::STORAGE_ERROR_NOT_READY;
#endif
}

// ---- Saving: the caller's text, unchanged until done -------------------------------------------------------

#ifdef USE_STORAGE_WORKER
inline void save_finish_(st::StorageError err) {
  Doc &d = doc();
  d.busy = false;
  d.src = nullptr;
  if (err == st::StorageError::STORAGE_ERROR_OK)
    d.dirty = false;
  Done done = std::move(d.save_done);
  d.save_done = nullptr;
  if (done)
    done(err);
}

inline void save_fail_(st::StorageError err) {
  st::global_storage_worker->end_write(doc().stream, [err](st::StorageError) {
    Doc &d = doc();
    if (!task_busy_(d.ps))
      d.ps->remove(d.tmp_rel.c_str());
    save_finish_(err);
  });
}

inline void save_step_() {
  Doc &d = doc();
  if (d.step >= d.len) {
    st::global_storage_worker->end_write(d.stream, [](st::StorageError e) {
      Doc &d = doc();
      if (e != st::StorageError::STORAGE_ERROR_OK) {
        save_finish_(e);
        return;
      }
      // Written whole: the old file goes, the new one takes its name (FAT's rename does not replace)
      if (task_busy_(d.ps)) {
        save_finish_(st::StorageError::STORAGE_ERROR_NOT_READY);
        return;
      }
      st::FileStat old{};
      if (d.ps->stat(d.rel.c_str(), &old) == st::StorageError::STORAGE_ERROR_OK) {
        const st::StorageError r = d.ps->remove(d.rel.c_str());
        if (r != st::StorageError::STORAGE_ERROR_OK) {
          save_finish_(r);
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
      save_finish_(r);
    });
    return;
  }
  const size_t n = std::min(CHUNK, d.len - d.step);
  const st::StorageError err = st::global_storage_worker->write_chunk(
      d.stream, reinterpret_cast<const uint8_t *>(d.src + d.step), n, [n](st::StorageError e) {
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          save_fail_(e);
          return;
        }
        doc().step += n;
        save_step_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK)
    save_fail_(err);
}
#endif

// Saves `text` (len bytes; it must stay unchanged until `done`) to `path`; the document is that file then.
inline st::StorageError save(const std::string &path, const char *text, size_t len, Done done) {
#ifdef USE_STORAGE_WORKER
  Doc &d = doc();
  if (d.busy || st::global_storage_worker == nullptr || text == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  std::string rel;
  st::PathStorage *ps = resolve_(path, &rel);
  if (ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy_(ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  d.path = path;
  d.ps = ps;
  d.rel = rel;
  d.tmp_rel = rel + ".tmp";
  d.src = text;
  d.len = len;
  d.step = 0;
  d.editable = true;
  d.binary = false;
  d.save_done = std::move(done);
  d.busy = true;
  const st::StorageError err = st::global_storage_worker->begin_write(ps, d.tmp_rel.c_str(), &d.stream, [](st::StorageError e) {
    if (e != st::StorageError::STORAGE_ERROR_OK) {
      save_finish_(e);
      return;
    }
    save_step_();
  });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    d.busy = false;
    d.src = nullptr;
    d.save_done = nullptr;
  }
  return err;
#else
  return st::StorageError::STORAGE_ERROR_NOT_READY;
#endif
}

// ---- Undo: one step, recorded from the text area's own insert events (no copy of the text) -----------------

struct Undo {
  uint32_t pos;          // character where the step happened
  uint32_t inserted;     // characters it put in at pos
  uint32_t removed_len;  // bytes it took out (kept in removed)
  uint32_t at_ms;
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

inline uint32_t utf8_chars_(const char *s, size_t n) {
  uint32_t c = 0;
  for (size_t i = 0; i < n; i++)
    c += (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80;
  return c;
}

// Before an insertion of `added` at character `cursor` of `text` (the text area's text now)
inline void note_insert(const char *text, uint32_t cursor, const char *added, uint32_t now_ms) {
  (void) text;
  Undo *u = undo_();
  if (u == nullptr || added == nullptr)
    return;
  const uint32_t n = utf8_chars_(added, strlen(added));
  if (u->valid && now_ms - u->at_ms < UNDO_MERGE_MS && cursor == u->pos + u->inserted) {
    u->inserted += n;  // typing on
  } else {
    u->pos = cursor;
    u->inserted = n;
    u->removed_len = 0;
    u->valid = true;
  }
  u->at_ms = now_ms;
}

// Before the character in front of `cursor` is deleted (backspace; a forward delete arrives the same way)
inline void note_delete(const char *text, uint32_t cursor, uint32_t now_ms) {
  Undo *u = undo_();
  if (u == nullptr || text == nullptr || cursor == 0)
    return;
  // The character's bytes: from the start of character cursor - 1 to the start of character cursor
  size_t i = 0, start = 0, end = 0;
  uint32_t c = 0;
  for (; text[i] != '\0'; i++) {
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) {
      if (c == cursor - 1)
        start = i;
      if (c == cursor) {
        end = i;
        break;
      }
      c++;
    }
  }
  if (text[i] == '\0')
    end = i;
  const size_t n = end - start;
  const bool recent = u->valid && now_ms - u->at_ms < UNDO_MERGE_MS;
  if (recent && u->inserted > 0 && cursor == u->pos + u->inserted) {
    u->inserted--;  // taking back what was just typed
  } else if (recent && u->inserted == 0 && cursor == u->pos && u->removed_len + n <= UNDO_MAX) {
    memmove(u->removed + n, u->removed, u->removed_len);  // backspacing on
    memcpy(u->removed, text + start, n);
    u->removed_len += n;
    u->pos = cursor - 1;
  } else if (n <= UNDO_MAX) {
    u->pos = cursor - 1;
    u->inserted = 0;
    memcpy(u->removed, text + start, n);
    u->removed_len = n;
    u->valid = true;
  } else {
    u->valid = false;
  }
  u->at_ms = now_ms;
}

inline bool can_undo() {
  Undo *u = undo_();
  return u != nullptr && u->valid && (u->inserted > 0 || u->removed_len > 0);
}

// The last step: characters to take out at pos (first), the text to put back there (then); false: nothing
inline bool take_undo(uint32_t *pos, uint32_t *remove, std::string *restore) {
  Undo *u = undo_();
  if (u == nullptr || !u->valid)
    return false;
  *pos = u->pos;
  *remove = u->inserted;
  restore->assign(u->removed, u->removed_len);
  u->valid = false;
  return true;
}

inline void forget_undo() {
  Undo *u = undo_();
  if (u != nullptr)
    u->valid = false;
}

// ---- Text helpers (any text; case-insensitive search for ASCII) --------------------------------------------

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

// The next match in `text` (len bytes) at or after `from` (forward) or before `from` (backward), wrapping
// round; -1 none.
inline int64_t find_in(const char *text, size_t len, const char *needle, size_t from, bool forward) {
  const size_t n = needle == nullptr ? 0 : strlen(needle);
  if (text == nullptr || n == 0 || n > len)
    return -1;
  const size_t count = len - n + 1;
  from = std::min(from, count);
  for (size_t k = 0; k < count; k++) {
    const size_t i = forward ? (from + k) % count : (from + 2 * count - 1 - k) % count;
    if (match_(text + i, needle, n))
      return static_cast<int64_t>(i);
  }
  return -1;
}

// `text` with every match replaced, into `out` (cap bytes, e.g. a borrowed buffer); its length, or -1 when
// it does not fit. *count: the matches replaced.
inline int64_t replace_all(const char *text, size_t len, const char *needle, const char *with, char *out, size_t cap,
                           size_t *count) {
  const size_t n = needle == nullptr ? 0 : strlen(needle), m = with == nullptr ? 0 : strlen(with);
  *count = 0;
  if (text == nullptr || out == nullptr || n == 0)
    return -1;
  size_t o = 0;
  for (size_t i = 0; i < len;) {
    if (i + n <= len && match_(text + i, needle, n)) {
      if (o + m >= cap)
        return -1;
      memcpy(out + o, with, m);
      o += m;
      i += n;
      (*count)++;
    } else {
      if (o + 1 >= cap)
        return -1;
      out[o++] = text[i++];
    }
  }
  out[o] = '\0';
  return static_cast<int64_t>(o);
}

inline size_t line_count(const char *text) {
  if (text == nullptr || text[0] == '\0')
    return 0;
  size_t n = 1;
  for (const char *p = text; *p != '\0'; p++)
    n += *p == '\n';
  return n;
}

// The byte where line `line` (0-based) starts
inline size_t line_start(const char *text, size_t line) {
  size_t pos = 0;
  for (size_t n = 0; text != nullptr && n < line && text[pos] != '\0'; pos++) {
    if (text[pos] == '\n')
      n++;
  }
  return pos;
}

// Line and column (1-based) of byte `pos`
inline void line_col(const char *text, size_t pos, size_t *line, size_t *col) {
  size_t l = 1, c = 1;
  for (size_t i = 0; text != nullptr && i < pos && text[i] != '\0'; i++) {
    if (text[i] == '\n') {
      l++;
      c = 1;
    } else {
      c++;
    }
  }
  *line = l;
  *col = c;
}

// UTF-8: the character index of byte `byte`, and the byte of character `index` (LVGL counts characters)
inline uint32_t char_index(const char *text, size_t byte) {
  uint32_t n = 0;
  for (size_t i = 0; text != nullptr && i < byte && text[i] != '\0'; i++)
    n += (static_cast<uint8_t>(text[i]) & 0xC0) != 0x80;
  return n;
}

inline size_t byte_of(const char *text, uint32_t index) {
  size_t i = 0;
  uint32_t n = 0;
  while (text != nullptr && text[i] != '\0') {
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) {
      if (n == index)
        return i;
      n++;
    }
    i++;
  }
  return i;
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

// The lines of `text` (in place: the buffer the text was read into) with a level up to `max_level`
// (1 E .. 6 V; lines without one always); returns the new length (NUL terminated).
inline size_t filter_levels(char *text, size_t len, uint8_t max_level) {
  size_t out = 0, i = 0;
  while (i < len) {
    size_t end = i;
    while (end < len && text[end] != '\n')
      end++;
    const size_t line = end - i + (end < len ? 1 : 0);
    if (line_level(text + i, end - i) <= max_level) {
      memmove(text + out, text + i, line);
      out += line;
    }
    i = end + 1;
  }
  text[out] = '\0';
  return out;
}

}  // namespace esphome::storage::text
