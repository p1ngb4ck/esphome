#pragma once

// WARNING: This component is EXPERIMENTAL. The API may change at any time
// without following the normal breaking changes policy. Use at your own risk.

// A browse model for a file manager UI on top of the storage interface and its worker: one page of a
// folder at a time (the rest counted, not held), sorted within the page; a selection; a clipboard and a
// paste that copies or moves through the worker, asking about entries already there; the single calls
// (mkdir, rename, delete, stat) guarded against the worker's task like the actions are. No UI here: a
// consumer (e.g. an LVGL page) draws browse::state() and calls these.
//
// RAM: the two pages, the clipboard and the delete list are PSRAM only (ALLOC_EXTERNAL, no internal
// fallback; ensure_memory() is false without PSRAM), taken while the page is open and given back when it
// is left (release_memory(); a clipboard holding something stays). Only State's few fields are internal.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <strings.h>

#include "esphome/core/helpers.h"
#include "esphome/core/time.h"
#include "storage.h"
#ifdef USE_STORAGE_WORKER
#include "storage_worker.h"
#endif

namespace esphome::storage::browse {

namespace st = esphome::storage;

static constexpr size_t PAGE_SIZE = 50;  // entries a page shows (and keeps)
static constexpr size_t CLIP_MAX = 32;   // entries the clipboard holds

struct Entry {
  char name[st::STORAGE_NAME_MAX + 1];
  uint64_t size;
  uint32_t mtime;
  bool is_dir;
};

// One page of a folder: its entries [first, first + count) in the storage's order, how many it has in
// all, and the order they are shown in (sorted within the page: a sort over the whole folder would
// need all of it held).
struct Page {
  Entry entries[PAGE_SIZE];
  uint8_t order[PAGE_SIZE];
  size_t count;
  size_t total;
  size_t first;
  bool ok;
};

struct Clip {
  char paths[CLIP_MAX][st::STORAGE_VFS_PATH_MAX];
  bool skipped[CLIP_MAX];  // already at the destination when pasted (asked about afterwards)
  size_t count;
  bool cut;
};

template<typename T> T *psram_new() {
  esphome::RAMAllocator<T> alloc(esphome::RAMAllocator<T>::ALLOC_EXTERNAL);
  T *p = alloc.allocate(1);
  if (p == nullptr)
    return nullptr;
  memset(static_cast<void *>(p), 0, sizeof(T));
  return p;
}

enum Mode : uint8_t { MODE_FOLDER = 0, MODE_STORAGE = 1, MODE_RAW = 2 };
enum JobKind : uint8_t { JOB_NONE = 0, JOB_PASTE, JOB_MOUNT, JOB_FORMAT, JOB_RAW };

struct State {
  // What is shown: a folder (full VFS path), a storage's panel (its mount path) or a raw device's (index)
  std::string dir;
  Mode mode{MODE_FOLDER};
  int raw_index{-1};
  size_t page{0};
  uint8_t sort{0};          // 0 name, 1 name down, 2 size, 3 size down, 4 date, 5 date down
  bool selecting{false};
  uint64_t selected{0};     // bits: entries of the page shown (by position in the page)
  Page *shown{nullptr};
  Page *listing{nullptr};   // being filled by the worker
  Clip *clip{nullptr};
  Clip *work{nullptr};      // what a delete goes through, one entry per loop pass
  size_t work_next{0};
  size_t work_failed{0};
  uint32_t list_gen{0};     // the listing asked for last; older completions are dropped
  bool list_busy{false};
  // The job running (one at a time)
  JobKind job{JOB_NONE};
#ifdef USE_STORAGE_WORKER
  st::TransferJob job_id{st::INVALID_TRANSFER_JOB};
#endif
  std::string job_text;     // what it does, for the transfer line
  bool job_overlay_hidden{false};  // the UI's job overlay put away by the user (until the job ends)
  uint64_t rate_done{0};    // the rate's last sample (rate()): bytes done then, and when
  uint32_t rate_ms{0};
  uint32_t rate_bps{0};     // bytes per second, smoothed
  std::string job_error;    // the last job's result ("" ok)
  size_t paste_next{0};     // paste: the clipboard entry going now
  bool paste_overwrite{false};
  size_t paste_skipped{0};
  size_t paste_failed{0};
  std::string paste_dir;    // where the paste goes (the folder shown when it started)
  // The name popup: 1 new folder, 2 rename (name_from the entry renamed)
  uint8_t name_mode{0};
  std::string name_from;
  // The raw device panel's choices
  bool raw_erase_first{true};
  uint8_t raw_verify{1};
  int raw_pending{0};       // the raw action waiting for its question (fm_click codes 41, 43, 44)
  void *fields[3]{};        // the raw panel's text fields: address, size, file (lv_obj_t *)
  bool ready() const {
    return this->shown != nullptr && this->listing != nullptr && this->clip != nullptr && this->work != nullptr;
  }
};

inline State &state() {
  static State s;
  return s;
}

// The PSRAM parts, on the page's first opening; false (and nothing taken) without PSRAM.
inline bool ensure_memory() {
  State &s = state();
  if (s.ready())
    return true;
  if (s.shown == nullptr)
    s.shown = psram_new<Page>();
  if (s.listing == nullptr)
    s.listing = psram_new<Page>();
  if (s.clip == nullptr)
    s.clip = psram_new<Clip>();
  if (s.work == nullptr)
    s.work = psram_new<Clip>();
  return s.ready();
}

template<typename T> void psram_delete(T *&p) {
  if (p == nullptr)
    return;
  esphome::RAMAllocator<T> alloc(esphome::RAMAllocator<T>::ALLOC_EXTERNAL);
  alloc.deallocate(p, 1);
  p = nullptr;
}

// The PSRAM parts given back when the page is left: the pages and the delete list once no listing or job
// uses them, the clipboard once it is empty (what is copied stays until it is pasted). ensure_memory()
// takes them again on the next opening.
inline void release_memory() {
  State &s = state();
  if (s.list_busy || s.job != JOB_NONE)
    return;
  psram_delete(s.shown);
  psram_delete(s.listing);
  psram_delete(s.work);
  if (s.clip != nullptr && s.clip->count == 0)
    psram_delete(s.clip);
  s.selecting = false;
  s.selected = 0;
}

// ---- text ----------------------------------------------------------------------------------------
inline std::string size_text(uint64_t bytes) {
  char out[24];
  if (bytes >= (1ULL << 30)) {
    snprintf(out, sizeof(out), "%.1f GB", static_cast<double>(bytes) / (1ULL << 30));
  } else if (bytes >= (1ULL << 20)) {
    snprintf(out, sizeof(out), "%.1f MB", static_cast<double>(bytes) / (1ULL << 20));
  } else if (bytes >= 1024) {
    snprintf(out, sizeof(out), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(out, sizeof(out), "%u B", static_cast<unsigned>(bytes));
  }
  return out;
}

inline std::string date_text(uint32_t mtime) {
  if (mtime == 0)
    return "";
  esphome::ESPTime t = esphome::ESPTime::from_epoch_local(static_cast<time_t>(mtime));
  return t.strftime("%d.%m.%Y %H:%M");
}

inline std::string lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return text;
}

inline std::string extension_of(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot == nullptr ? std::string() : lower(dot);
}

inline std::string join(const std::string &dir, const char *name) {
  return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

inline std::string parent_of(const std::string &path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

inline const char *base_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}

// ---- storages --------------------------------------------------------------------------------------
// A full VFS path -> its storage and the path within it ("/" for the mount point itself).
inline st::PathStorage *resolve(const std::string &path, std::string *rel) {
  if (st::global_storage_registry == nullptr)
    return nullptr;
  const char *r = nullptr;
  st::PathStorage *ps = st::global_storage_registry->resolve_path(path.c_str(), &r);
  if (ps != nullptr && rel != nullptr)
    *rel = (r == nullptr || r[0] == '\0') ? std::string("/") : std::string(r);
  return ps;
}

// The worker task is inside this storage now: a call from here would put two threads into it.
inline bool task_busy(const st::Storage *s) {
#ifdef USE_STORAGE_WORKER
  return st::global_storage_worker != nullptr && st::global_storage_worker->has_active_task_io(s);
#else
  return false;
#endif
}

inline bool busy_with(const st::Storage *s) {
#ifdef USE_STORAGE_WORKER
  return st::global_storage_worker != nullptr && st::global_storage_worker->is_busy_with(s);
#else
  return false;
#endif
}

// The path storages, by position (for_each_path_based's order)
inline st::PathStorage *path_storage(int index) {
  if (st::global_storage_registry == nullptr || index < 0)
    return nullptr;
  struct Find {
    int want, at;
    st::PathStorage *found;
  } find{index, 0, nullptr};
  st::global_storage_registry->for_each_path_based(
      [](st::PathStorage *s, void *ctx) {
        auto *f = static_cast<Find *>(ctx);
        if (f->at++ == f->want)
          f->found = s;
      },
      &find);
  return find.found;
}

// The first mounted storage's top folder ("" none)
inline std::string first_root() {
  std::string root;
  if (st::global_storage_registry == nullptr)
    return root;
  st::global_storage_registry->for_each_path_based(
      [](st::PathStorage *s, void *ctx) {
        auto *out = static_cast<std::string *>(ctx);
        st::StorageInfo info{};
        if (out->empty() && s->get_info(&info) == st::StorageError::STORAGE_ERROR_OK && info.is_mounted)
          *out = s->get_mount_path();
      },
      &root);
  return root;
}

// A storage's own top folder (or no storage at all)
inline bool at_root(const std::string &dir) {
  std::string rel;
  return resolve(dir, &rel) == nullptr || rel == "/";
}

// Back has somewhere to go on the page: from a panel to its folder, or up a folder
inline bool can_go_back() {
  const State &s = state();
  return s.mode != MODE_FOLDER || !at_root(s.dir);
}

// The raw devices, by position (for_each_raw's order)
inline st::RawStorage *raw_device(int index) {
  if (st::global_storage_registry == nullptr || index < 0)
    return nullptr;
  struct Find {
    int want, at;
    st::RawStorage *found;
  } find{index, 0, nullptr};
  st::global_storage_registry->for_each_raw(
      [](st::RawStorage *s, void *ctx) {
        auto *f = static_cast<Find *>(ctx);
        if (f->at++ == f->want)
          f->found = s;
      },
      &find);
  return find.found;
}

// ---- listing ---------------------------------------------------------------------------------------
inline void sort_page(Page *page, uint8_t mode) {
  for (size_t i = 0; i < page->count; i++)
    page->order[i] = static_cast<uint8_t>(i);
  const Entry *e = page->entries;
  std::stable_sort(page->order, page->order + page->count, [e, mode](uint8_t a, uint8_t b) {
    if (e[a].is_dir != e[b].is_dir)
      return e[a].is_dir;  // folders first
    switch (mode) {
      case 2:
      case 3:
        if (e[a].size != e[b].size)
          return mode == 2 ? e[a].size < e[b].size : e[a].size > e[b].size;
        break;
      case 4:
      case 5:
        if (e[a].mtime != e[b].mtime)
          return mode == 4 ? e[a].mtime < e[b].mtime : e[a].mtime > e[b].mtime;
        break;
      default:
        break;
    }
    const int c = strcasecmp(e[a].name, e[b].name);
    return mode == 1 ? c > 0 : c < 0;
  });
}

// Asks the worker for the folder's page `page`; on_done(ok) on the main loop once s.shown holds it.
// false if it could not be asked (no storage, the worker's queue full, its task inside the storage).
inline bool list(const std::string &dir, size_t page, std::function<void(bool)> on_done) {
#ifdef USE_STORAGE_WORKER
  State &s = state();
  std::string rel;
  st::PathStorage *ps = resolve(dir, &rel);
  if (ps == nullptr || st::global_storage_worker == nullptr || !s.ready())
    return false;
  Page *fill = s.listing;
  fill->count = 0;
  fill->total = 0;
  fill->first = page * PAGE_SIZE;
  fill->ok = false;
  const uint32_t gen = ++s.list_gen;
  // On the worker task: only `fill` is touched (the shown page stays as it is till the completion).
  auto entry_cb = [](const st::FileStat *entry, void *ctx) -> bool {
    auto *p = static_cast<Page *>(ctx);
    const size_t index = p->total++;
    if (index >= p->first && p->count < PAGE_SIZE) {
      Entry &e = p->entries[p->count++];
      strncpy(e.name, entry->name, sizeof(e.name) - 1);
      e.name[sizeof(e.name) - 1] = '\0';
      e.size = entry->size;
      e.mtime = entry->mtime;
      e.is_dir = entry->is_dir;
    }
    return true;
  };
  const st::StorageError err = st::global_storage_worker->async_list_dir(
      ps, rel.c_str(), entry_cb, fill, [gen, on_done](st::StorageError result) {
        State &s = state();
        if (gen != s.list_gen)
          return;  // another folder or page asked for meanwhile
        s.list_busy = false;
        s.listing->ok = result == st::StorageError::STORAGE_ERROR_OK;
        std::swap(s.shown, s.listing);
        sort_page(s.shown, s.sort);
        on_done(s.shown->ok);
      });
  s.list_busy = err == st::StorageError::STORAGE_ERROR_OK;
  return s.list_busy;
#else
  return false;
#endif
}

// Feeds the job's bytes done (from the worker's progress) and returns the smoothed bytes per second;
// a new job (done went back) starts over.
inline uint32_t rate(uint64_t done, uint32_t now_ms) {
  State &s = state();
  if (done < s.rate_done || s.rate_ms == 0) {
    s.rate_done = done;
    s.rate_ms = now_ms;
    s.rate_bps = 0;
    return 0;
  }
  const uint32_t dt = now_ms - s.rate_ms;
  if (dt >= 200) {
    const uint32_t now_bps = static_cast<uint32_t>((done - s.rate_done) * 1000 / dt);
    s.rate_bps = s.rate_bps == 0 ? now_bps : (s.rate_bps * 3 + now_bps) / 4;
    s.rate_done = done;
    s.rate_ms = now_ms;
  }
  return s.rate_bps;
}

inline size_t page_count() {
  const Page *p = state().shown;
  return p == nullptr || p->total == 0 ? 1 : (p->total + PAGE_SIZE - 1) / PAGE_SIZE;
}

// The entry shown at row `row` of the page
inline const Entry *entry_at(size_t row) {
  const Page *p = state().shown;
  if (p == nullptr || row >= p->count)
    return nullptr;
  return &p->entries[p->order[row]];
}

inline size_t selected_count() {
  const State &s = state();
  size_t n = 0;
  for (size_t i = 0; s.shown != nullptr && i < s.shown->count; i++)
    n += (s.selected >> i) & 1;
  return n;
}

// ---- single operations (main loop, guarded) ---------------------------------------------------------
inline st::StorageError make_dir(const std::string &dir, const std::string &name) {
  std::string rel;
  st::PathStorage *ps = resolve(join(dir, name.c_str()), &rel);
  if (ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy(ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  const st::StorageError err = ps->mkdir(rel.c_str());
#ifdef USE_STORAGE_CHANGE_FEED
  if (err == st::StorageError::STORAGE_ERROR_OK)
    st::global_storage_registry->note_dir_changed(dir);
#endif
  return err;
}

inline st::StorageError rename_entry(const std::string &dir, const char *from, const std::string &to) {
  std::string rel_from, rel_to;
  st::PathStorage *ps = resolve(join(dir, from), &rel_from);
  st::PathStorage *ps_to = resolve(join(dir, to.c_str()), &rel_to);
  if (ps == nullptr || ps != ps_to)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy(ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  const st::StorageError err = ps->rename(rel_from.c_str(), rel_to.c_str());
#ifdef USE_STORAGE_CHANGE_FEED
  if (err == st::StorageError::STORAGE_ERROR_OK)
    st::global_storage_registry->note_dir_changed(dir);
#endif
  return err;
}

// Deletes one entry (a folder with everything in it)
inline st::StorageError delete_entry(const std::string &path) {
  std::string rel;
  st::PathStorage *ps = resolve(path, &rel);
  if (ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy(ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  st::FileStat info{};
  st::StorageError err = ps->stat(rel.c_str(), &info);
  if (err != st::StorageError::STORAGE_ERROR_OK)
    return err;
  err = info.is_dir ? st::remove_recursive(ps, rel.c_str()) : ps->remove(rel.c_str());
#ifdef USE_STORAGE_CHANGE_FEED
  st::global_storage_registry->note_parent_changed(path);
#endif
  return err;
}

inline st::StorageError stat_entry(const std::string &path, st::FileStat *out) {
  std::string rel;
  st::PathStorage *ps = resolve(path, &rel);
  if (ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  if (task_busy(ps))
    return st::StorageError::STORAGE_ERROR_NOT_READY;
  return ps->stat(rel.c_str(), out);
}

// ---- clipboard and paste (worker) ---------------------------------------------------------------------
// The page's selected entries into `c` (the clipboard: cut = moved on paste). Returns how many.
inline size_t take_selected(Clip *c, bool cut) {
  State &s = state();
  if (!s.ready() || c == nullptr)
    return 0;
  c->count = 0;
  c->cut = cut;
  for (size_t row = 0; row < s.shown->count && c->count < CLIP_MAX; row++) {
    if (!((s.selected >> row) & 1))
      continue;
    const Entry *e = entry_at(row);
    const std::string path = join(s.dir, e->name);
    if (path.size() >= st::STORAGE_VFS_PATH_MAX)
      continue;
    memcpy(c->paths[c->count], path.c_str(), path.size() + 1);
    c->skipped[c->count] = false;
    c->count++;
  }
  return c->count;
}

#ifdef USE_STORAGE_WORKER
// Submits clipboard entry s.paste_next (onto dst_dir); on_done(error) on the main loop.
inline st::StorageError paste_one(const std::string &dst_dir, std::function<void(st::StorageError)> on_done) {
  State &s = state();
  const char *src = s.clip->paths[s.paste_next];
  std::string src_rel, dst_rel;
  st::PathStorage *src_ps = resolve(src, &src_rel);
  st::PathStorage *dst_ps = resolve(join(dst_dir, base_of(src)), &dst_rel);
  if (src_ps == nullptr || dst_ps == nullptr)
    return st::StorageError::STORAGE_ERROR_NOT_FOUND;
  // Into itself (or a folder below it): refused, as cp does
  const std::string dst_full = join(dst_dir, base_of(src));
  if (dst_full == src || dst_full.rfind(std::string(src) + "/", 0) == 0)
    return st::StorageError::STORAGE_ERROR_INVALID_ARGS;
  auto cb = [on_done](st::StorageError e) { on_done(e); };
  return s.clip->cut ? st::global_storage_worker->async_move(src_ps, src_rel.c_str(), dst_ps, dst_rel.c_str(), cb,
                                                            &s.job_id, s.paste_overwrite)
                     : st::global_storage_worker->async_copy(src_ps, src_rel.c_str(), dst_ps, dst_rel.c_str(), cb,
                                                            &s.job_id, s.paste_overwrite);
}
#endif

// ---- the raw panel's numbers -------------------------------------------------------------------------
// "0x..." hex or decimal, with an optional K / M suffix (x1024); false if it is not a number
inline bool parse_number(const char *text, uint64_t *out) {
  while (*text == ' ')
    text++;
  if (*text == '\0')
    return false;
  char *end = nullptr;
  uint64_t v = strtoull(text, &end, 0);
  if (end == text)
    return false;
  if (*end == 'k' || *end == 'K') {
    v *= 1024;
    end++;
  } else if (*end == 'm' || *end == 'M') {
    v *= 1024 * 1024;
    end++;
  }
  while (*end == ' ' || *end == 'B' || *end == 'b')
    end++;
  if (*end != '\0')
    return false;
  *out = v;
  return true;
}

}  // namespace esphome::storage::browse
