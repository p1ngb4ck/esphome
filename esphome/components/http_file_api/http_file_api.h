#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/storage/storage.h"
// storage_worker.h is itself fully USE_STORAGE_WORKER-gated (including class StorageWorker),
// so it's always safe to include — on a build with no async-capable driver configured, it
// contributes nothing and global_storage_worker is simply never referenced.
#include "esphome/components/storage/storage_worker.h"
#include <cstdint>

namespace esphome::http_file_api {

// Bounded directory-listing result — capacity fixed at codegen time (max_dir_entries),
// no heap growth. Mirrors the old http_file_browser's MAX_DIR_ENTRIES cap but via
// FixedVector so overflow is silently truncated rather than needing a raw-array bound
// check at every call site.
using DirEntries = FixedVector<storage::FileStat>;

// Thin, storage.h-native replacement for the file-operation half of the old
// http_file_browser component. Contains no HTTP/HTML code at all — see
// web_server_filebrowser for the AsyncWebHandler adapter that consumes this API.
//
// Scoping: if storage_ is set (single-instance mode), every call defaults to it when the
// caller passes storage == nullptr. If storage_ is nullptr (registry-wide mode), the
// caller must either pass an explicit PathStorage or a path resolvable via
// storage::global_storage_registry->resolve_path().
class HttpFileApi : public Component {
 public:
  void set_storage(storage::PathStorage *storage) { this->storage_ = storage; }
  void set_max_dir_entries(size_t max_dir_entries) { this->max_dir_entries_ = max_dir_entries; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  storage::StorageError list_dir(const char *path, DirEntries &out, storage::PathStorage *storage = nullptr);
  storage::StorageError stat(const char *path, storage::FileStat *out, storage::PathStorage *storage = nullptr);
  storage::StorageError read_file(const char *path, storage::RamBuffer &out, size_t *size,
                                  storage::PathStorage *storage = nullptr);
  storage::StorageError write_file(const char *path, const uint8_t *data, size_t size,
                                   storage::PathStorage *storage = nullptr);
  storage::StorageError remove(const char *path, bool recursive, storage::PathStorage *storage = nullptr);
  storage::StorageError mkdir(const char *path, storage::PathStorage *storage = nullptr);
  storage::StorageError rename(const char *old_path, const char *new_path, storage::PathStorage *storage = nullptr);
  storage::StorageError get_storage_info(storage::StorageInfo *out, storage::PathStorage *storage = nullptr);

  // Transparent copy/move: uses StorageWorker's async_copy()/async_move() when the async
  // worker is compiled in and available (global_storage_worker != nullptr) — non-blocking,
  // on_copy_complete_/on_move_complete_ fire later from the worker's completion callback.
  // Otherwise falls straight through to storage::copy()/move(), synchronous and BLOCKING,
  // firing on_copy_complete_/on_move_complete_ immediately with the result. Either way the
  // call signature and callback contract are identical — callers cannot tell which path
  // ran except by timing, matching storage_worker.h's own transparency guarantee.
  storage::StorageError copy(const char *src_path, const char *dst_path, storage::PathStorage *src_storage = nullptr,
                             storage::PathStorage *dst_storage = nullptr);
  storage::StorageError move(const char *src_path, const char *dst_path, storage::PathStorage *src_storage = nullptr,
                             storage::PathStorage *dst_storage = nullptr);

  // Automation callbacks — templatized so both std::function and pointer-sized
  // forwarder structs are accepted without forcing heap allocation.
  template<typename F> void add_on_upload_complete_callback(F &&cb) {
    this->on_upload_complete_.add(std::forward<F>(cb));
  }
  template<typename F> void add_on_delete_callback(F &&cb) { this->on_delete_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_mkdir_callback(F &&cb) { this->on_mkdir_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_rename_callback(F &&cb) { this->on_rename_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_copy_complete_callback(F &&cb) { this->on_copy_complete_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_move_complete_callback(F &&cb) { this->on_move_complete_.add(std::forward<F>(cb)); }

 protected:
  // Resolves the effective PathStorage for a call: explicit override, else this->storage_.
  // Returns nullptr if neither is available (registry-wide mode with no override given).
  storage::PathStorage *resolve_storage_(storage::PathStorage *override_storage) const {
    return override_storage != nullptr ? override_storage : this->storage_;
  }

  storage::PathStorage *storage_{nullptr};
  size_t max_dir_entries_{256};

  LazyCallbackManager<void(const char *)> on_upload_complete_;
  LazyCallbackManager<void(const char *)> on_delete_;
  LazyCallbackManager<void(const char *)> on_mkdir_;
  LazyCallbackManager<void(const char *, const char *)> on_rename_;
  LazyCallbackManager<void(storage::StorageError)> on_copy_complete_;
  LazyCallbackManager<void(storage::StorageError)> on_move_complete_;
};

}  // namespace esphome::http_file_api
