#pragma once

// defines.h MUST be seen before the guard below is evaluated: the preferences
// action classes are define-gated, and main.cpp placement-news them into
// sizeof()-sized static buffers — any TU seeing this header with a different
// define state gets a different class size (ODR violation, boot crash).
#include "esphome/core/defines.h"

#include "storage.h"
#ifdef USE_STORAGE_WORKER
#include "storage_worker.h"  // global_storage_worker — async file/raw ops (fire-and-forget actions)
#endif
#if defined(USE_STORAGE_PREFERENCES) && defined(USE_ESP32)
#include "preferences_backup.h"  // PrefSelection — file scope, NOT inside the namespace below
#endif
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include <string>
#include <vector>

namespace esphome::storage {

// These actions are globally available on every node that loads the storage component
// (which every storage device driver AUTO_LOADs) — no per-component preparation needed,
// analogous to how web_server sorting groups work for all components. Paths are full VFS
// paths; routing to the right device happens via StorageRegistry::resolve_path().

// Logging helpers implemented in automation.cpp (log macros must stay out of headers).
// printf-style args from YAML flow verbatim through C varargs in the generated
// str_sprintf() call; a std::string there is undefined behavior (non-POD through
// "..." renders garbage or corrupts memory, and only warns via -Wformat).
// Normalizing every arg through this overload set means no config ever needs a
// manual .c_str() — and args that already have one pass through unchanged.
inline const char *printf_arg(const std::string &s) { return s.c_str(); }
template<typename T> inline T printf_arg(T v) { return v; }

void warn_invalid_bool(const std::string &s);
void warn_invalid_number(const std::string &s);

// Assign an extracted string to a global variable of any supported type. The value type is
// deduced from the global's value() so one helper covers GlobalsComponent<T>,
// RestoringGlobalsComponent<T> and the string variants alike.
template<typename GlobT> void assign_from_string(GlobT *g, const std::string &s) {
  using T = std::decay_t<decltype(g->value())>;
  if constexpr (std::is_same_v<T, std::string>) {
    g->value() = s;
  } else if constexpr (std::is_same_v<T, bool>) {
    if (str_equals_case_insensitive(s, "true") || str_equals_case_insensitive(s, "on") || s == "1") {
      g->value() = true;
    } else if (str_equals_case_insensitive(s, "false") || str_equals_case_insensitive(s, "off") || s == "0") {
      g->value() = false;
    } else {
      warn_invalid_bool(s);
    }
  } else if constexpr (std::is_arithmetic_v<T>) {
    auto v = parse_number<T>(s);
    if (v.has_value()) {
      g->value() = *v;
    } else {
      warn_invalid_number(s);
    }
  } else {
    static_assert(std::is_same_v<T, std::string>, "Unsupported global type for storage.file_read to_global");
  }
}

// ---------------------------------------------------------------------------
// Extraction pipeline for storage.file_read (implementation in automation.cpp)
// ---------------------------------------------------------------------------

enum class ExtractStepType : uint8_t {
  LINE,   // pick the Nth line (1-based)
  SPLIT,  // split at a separator string, pick the Nth element (0-based)
  JSON,   // resolve a '/'-separated pointer (object keys, array indices) in a JSON document
  KEY,    // find the first line starting with "<key><separator>", yield the remainder
  TRIM,   // strip leading/trailing whitespace
  REGEX,  // regex_search, yield the given capture group
};

struct ExtractStep {
  ExtractStepType type;
  std::string arg;  // SPLIT: separator, KEY: key, REGEX: pattern
  std::string sep;  // KEY: separator
  int index{0};     // LINE: line number, SPLIT: element index, REGEX: capture group
};

// Whitespace trim (no equivalent in core helpers).
std::string extract_trim(const std::string &s);

// Applies one step; returns false (with a warning) on structural failure — line/element out
// of range, key not found, regex not matching. An empty extraction result is not a failure.
bool apply_extract_step(const ExtractStep &step, std::string &buf);

// Non-template workers for the actions below — all error logging lives in the .cpp.
void perform_mount(MountableStorage *target, bool mount);
void perform_file_copy(const std::string &from, const std::string &to, bool is_move);
// Async variant used by FileCopyAction: submits to the worker (or, if the worker is not
// compiled in, runs the blocking helper and fires the trigger inline). `on_complete` receives
// the error text (empty = success) and may be nullptr.
void perform_file_copy_async(const std::string &from, const std::string &to, bool is_move,
                             Trigger<std::string> *on_complete);
void perform_file_delete(const std::string &path, bool recursive);
bool check_file_exists(const std::string &path);
void perform_file_write(const std::string &path, std::string content, bool append, bool newline);
bool perform_file_read(const std::string &path, const std::vector<ExtractStep> &steps, std::string &out);

// ---------------------------------------------------------------------------
// storage.file_write / storage.file_append
// ---------------------------------------------------------------------------

template<typename... Ts> class FileWriteAction : public Action<Ts...> {
 public:
  explicit FileWriteAction(bool append) : append_(append) {}

  TEMPLATABLE_VALUE(std::string, path)
  TEMPLATABLE_VALUE(std::string, content)
  void set_newline(bool newline) { this->newline_ = newline; }

  void play(const Ts &...x) override {
    perform_file_write(this->path_.value(x...), this->content_.value(x...), this->append_, this->newline_);
  }

 protected:
  bool append_;
  bool newline_{false};
};

// ---------------------------------------------------------------------------
// storage.file_read
// ---------------------------------------------------------------------------

template<typename... Ts> class FileReadAction : public Action<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, path)

  void add_step(ExtractStepType type, std::string arg, std::string sep, int index) {
    this->steps_.push_back(ExtractStep{type, std::move(arg), std::move(sep), index});
  }
  void set_global_setter(std::function<void(const std::string &)> setter) { this->setter_ = std::move(setter); }
  Trigger<std::string> *get_value_trigger() { return &this->value_trigger_; }

  void play(const Ts &...x) override {
    std::string value;
    if (!perform_file_read(this->path_.value(x...), this->steps_, value))
      return;  // already logged; global untouched, no trigger
    if (this->setter_)
      this->setter_(value);
    this->value_trigger_.trigger(value);
  }

 protected:
  std::vector<ExtractStep> steps_;
  std::function<void(const std::string &)> setter_;
  Trigger<std::string> value_trigger_;
};

// ---------------------------------------------------------------------------
// storage.file_copy / storage.file_move (move doubles as rename — see .cpp)
// ---------------------------------------------------------------------------

// Fire-and-forget: play() submits the copy/move to the async worker and returns immediately,
// so the action sequence continues without blocking the loop for the transfer's duration. The
// on_complete trigger fires later from the worker's completion callback (main loop) with the
// error text — empty string on success. A same-storage move still takes the rename() fast path
// inside the worker's pre-phase. Falls back to the synchronous helper only when the worker was
// not compiled in (no path driver requested it); that path blocks, as before.
template<typename... Ts> class FileCopyAction : public Action<Ts...> {
 public:
  explicit FileCopyAction(bool is_move) : is_move_(is_move) {}

  TEMPLATABLE_VALUE(std::string, from)
  TEMPLATABLE_VALUE(std::string, to)

  Trigger<std::string> *get_complete_trigger() { return &this->complete_trigger_; }

  void play(const Ts &...x) override {
    perform_file_copy_async(this->from_.value(x...), this->to_.value(x...), this->is_move_, &this->complete_trigger_);
  }

 protected:
  bool is_move_;
  Trigger<std::string> complete_trigger_;
};

#ifdef USE_STORAGE_RAW_ACTIONS
// ---------------------------------------------------------------------------
// storage.raw_read / storage.raw_write / storage.raw_erase
// ---------------------------------------------------------------------------
// Address-based access to a RawStorage device (NOR flash, FRAM, EEPROM). What a medium
// accepts is its own business — these helpers ask get_raw_geometry() instead of assuming
// flash semantics, and pass erase()'s verdict (NOT_SUPPORTED on erase-less media, INVALID_ARGS
// on an unaligned range) straight through to the log rather than papering over it.
//
// Like the file_* actions above these are BLOCKING and honor max_blocking_transfer_size.

// Reads [address, address+size) into `out`. Returns false (already logged) on any failure;
// `out` is left empty then, so a trigger never fires with half a result.
bool perform_raw_read(RawStorage *device, uint64_t address, size_t size, std::vector<uint8_t> &out);
// Same, but streams into a file on a mounted storage. size == 0 means "to the end of the device".
bool perform_raw_read_to_file(RawStorage *device, uint64_t address, uint64_t size, const std::string &path);
// Writes `data` at `address`. erase_first erases the covering sectors beforehand — required on
// media reporting RAW_WRITE_NEEDS_ERASE, and destructive to anything else sharing those sectors.
bool perform_raw_write(RawStorage *device, uint64_t address, const uint8_t *data, size_t len, bool erase_first);
bool perform_raw_write_from_file(RawStorage *device, uint64_t address, const std::string &path, bool erase_first);
// Erases [address, address+size), or the whole device when `all` is set.
void perform_raw_erase(RawStorage *device, uint64_t address, uint64_t size, bool all);

template<typename... Ts> class RawReadAction : public Action<Ts...> {
 public:
  explicit RawReadAction(RawStorage *device) : device_(device) {}

  TEMPLATABLE_VALUE(uint32_t, address)
  TEMPLATABLE_VALUE(uint32_t, size)
  TEMPLATABLE_VALUE(std::string, to_file)

  void set_has_to_file(bool has_to_file) { this->has_to_file_ = has_to_file; }
  Trigger<std::vector<uint8_t>> *get_value_trigger() { return &this->value_trigger_; }

  void play(const Ts &...x) override {
    const uint32_t address = this->address_.value(x...);
    const uint32_t size = this->size_.value(x...);
    if (this->has_to_file_) {
      perform_raw_read_to_file(this->device_, address, size, this->to_file_.value(x...));
      return;
    }
    std::vector<uint8_t> data;
    if (!perform_raw_read(this->device_, address, size, data))
      return;  // already logged; no trigger on a failed read
    this->value_trigger_.trigger(data);
  }

 protected:
  RawStorage *device_;
  bool has_to_file_{false};
  Trigger<std::vector<uint8_t>> value_trigger_;
};

template<typename... Ts> class RawWriteAction : public Action<Ts...> {
 public:
  explicit RawWriteAction(RawStorage *device) : device_(device) {}

  TEMPLATABLE_VALUE(uint32_t, address)
  TEMPLATABLE_VALUE(std::string, from_file)

  // Data sources, mirroring uart.write: a static array stays in flash, a lambda is called per
  // play(). from_file reads the file into RAM first (guard-railed by the transfer limit).
  void set_data_template(std::vector<uint8_t> (*func)(Ts...)) {
    this->code_.func = func;
    this->len_ = -1;  // sentinel: template mode
  }
  void set_data_static(const uint8_t *data, size_t len) {
    this->code_.data = data;
    this->len_ = static_cast<int>(len);
  }
  void set_has_from_file(bool has_from_file) { this->has_from_file_ = has_from_file; }
  void set_erase_first(bool erase_first) { this->erase_first_ = erase_first; }

  void play(const Ts &...x) override {
    const uint32_t address = this->address_.value(x...);
    if (this->has_from_file_) {
      perform_raw_write_from_file(this->device_, address, this->from_file_.value(x...), this->erase_first_);
      return;
    }
    if (this->len_ >= 0) {
      perform_raw_write(this->device_, address, this->code_.data, static_cast<size_t>(this->len_), this->erase_first_);
      return;
    }
    std::vector<uint8_t> data = (*this->code_.func)(x...);
    perform_raw_write(this->device_, address, data.data(), data.size(), this->erase_first_);
  }

 protected:
  RawStorage *device_;
  union {
    const uint8_t *data;
    std::vector<uint8_t> (*func)(Ts...);
  } code_{nullptr};
  int len_{-1};
  bool has_from_file_{false};
  bool erase_first_{false};
};

template<typename... Ts> class RawEraseAction : public Action<Ts...> {
 public:
  explicit RawEraseAction(RawStorage *device) : device_(device) {}

  TEMPLATABLE_VALUE(uint32_t, address)
  TEMPLATABLE_VALUE(uint32_t, size)

  void set_all(bool all) { this->all_ = all; }

  void play(const Ts &...x) override {
    perform_raw_erase(this->device_, this->address_.value(x...), this->size_.value(x...), this->all_);
  }

 protected:
  RawStorage *device_;
  bool all_{false};
};

#endif  // USE_STORAGE_RAW_ACTIONS

// ---------------------------------------------------------------------------
// storage.file_delete
// ---------------------------------------------------------------------------

template<typename... Ts> class FileDeleteAction : public Action<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void set_recursive(bool recursive) { this->recursive_ = recursive; }

  void play(const Ts &...x) override { perform_file_delete(this->path_.value(x...), this->recursive_); }

 protected:
  bool recursive_{false};
};

// ---------------------------------------------------------------------------
// storage.file_exists condition
// ---------------------------------------------------------------------------

template<typename... Ts> class FileExistsCondition : public Condition<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, path)

  bool check(const Ts &...x) override { return check_file_exists(this->path_.value(x...)); }
};

// ---------------------------------------------------------------------------
// storage.mount / storage.unmount — target must opt in via MountableStorage
// (validated at YAML time through the codegen class hierarchy)
// ---------------------------------------------------------------------------

template<typename... Ts> class MountAction : public Action<Ts...> {
 public:
  explicit MountAction(MountableStorage *target, bool mount) : target_(target), mount_(mount) {}

  void play(const Ts &...x) override { perform_mount(this->target_, this->mount_); }

 protected:
  MountableStorage *target_;
  bool mount_;
};

#if defined(USE_STORAGE_PREFERENCES) && defined(USE_ESP32)
// storage.export_preferences / storage.import_preferences — see
// preferences_backup.h. The selection table (name/key/type/count) is
// codegen-baked per action instance from its optional `preferences:` list;
// empty selection = all preferences (hex round-trip, types unknown).
// The two preference actions take either a path on a mounted storage (rendered, kv/json) or a
// raw device plus address (the encoded blob as stored). Codegen picks exactly one and hands the
// raw variant its window — the room up to the next region on that device, 0 meaning "to the end
// of the device", which only the device itself knows.
template<typename... Ts> class ExportPreferencesAction : public Action<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void set_format(const char *format) { this->format_ = format; }
  void set_raw_target(RawStorage *device, uint32_t address, uint32_t window) {
    this->device_ = device;
    this->address_ = address;
    this->window_ = window;
  }
  void set_selection(const PrefSelection *selection, size_t count, bool restrict_to_selection) {
    this->selection_ = selection;
    this->count_ = count;
    this->restrict_ = restrict_to_selection;
  }
  void add_selected_entity(esphome::EntityBase *entity) { this->selected_entities_.push_back(entity); }

  void play(Ts... x) override {
    if (this->device_ != nullptr) {
      preferences_export_to_raw(this->device_, this->address_, this->resolved_window_(), this->selection_, this->count_,
                                this->restrict_, this->selected_entities_.data(), this->selected_entities_.size());
      return;
    }
    const std::string path = this->path_.value(x...);
    preferences_export_to_storage(path.c_str(), this->format_, this->selection_, this->count_, this->restrict_,
                                  this->selected_entities_.data(), this->selected_entities_.size());
  }

 protected:
  // window 0 = the last region on this device: everything from here to the end of it.
  uint64_t resolved_window_() {
    if (this->window_ != 0)
      return this->window_;
    RawGeometry geo;
    this->device_->get_raw_geometry(&geo);
    return geo.capacity > this->address_ ? geo.capacity - this->address_ : 0;
  }

  RawStorage *device_{nullptr};
  uint32_t address_{0};
  uint32_t window_{0};
  const char *format_{"kv"};
  const PrefSelection *selection_{nullptr};
  size_t count_{0};
  bool restrict_{false};
  std::vector<esphome::EntityBase *> selected_entities_;
};

template<typename... Ts> class ImportPreferencesAction : public Action<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void set_raw_target(RawStorage *device, uint32_t address, uint32_t window) {
    this->device_ = device;
    this->address_ = address;
    this->window_ = window;
  }
  void set_format(const char *format) { this->format_ = format; }
  void set_reboot(bool reboot) { this->reboot_ = reboot; }
  void set_selection(const PrefSelection *selection, size_t count, bool restrict_to_selection) {
    this->selection_ = selection;
    this->count_ = count;
    this->restrict_ = restrict_to_selection;
  }
  void add_selected_entity(esphome::EntityBase *entity) { this->selected_entities_.push_back(entity); }

  void play(Ts... x) override {
    if (this->device_ != nullptr) {
      preferences_import_from_raw(this->device_, this->address_, this->resolved_window_(), this->reboot_,
                                  this->selection_, this->count_, this->restrict_, this->selected_entities_.data(),
                                  this->selected_entities_.size());
      return;
    }
    const std::string path = this->path_.value(x...);
    preferences_import_from_storage(path.c_str(), this->format_, this->reboot_, this->selection_, this->count_,
                                    this->restrict_, this->selected_entities_.data(), this->selected_entities_.size());
  }

 protected:
  // window 0 = the last region on this device: everything from here to the end of it.
  uint64_t resolved_window_() {
    if (this->window_ != 0)
      return this->window_;
    RawGeometry geo;
    this->device_->get_raw_geometry(&geo);
    return geo.capacity > this->address_ ? geo.capacity - this->address_ : 0;
  }

  RawStorage *device_{nullptr};
  uint32_t address_{0};
  uint32_t window_{0};
  const char *format_{"kv"};
  bool reboot_{false};
  const PrefSelection *selection_{nullptr};
  size_t count_{0};
  bool restrict_{false};
  std::vector<esphome::EntityBase *> selected_entities_;
};
#endif  // USE_STORAGE_PREFERENCES && USE_ESP32

}  // namespace esphome::storage
