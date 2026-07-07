#include "http_file_api.h"
#include "esphome/core/log.h"

namespace esphome::http_file_api {

static const char *const TAG = "http_file_api";

void HttpFileApi::setup() {}

void HttpFileApi::dump_config() {
  ESP_LOGCONFIG(TAG, "HTTP File API:");
  if (this->storage_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Storage mount path: %s", this->storage_->get_mount_path());
  } else {
    ESP_LOGCONFIG(TAG, "  Mode: registry-wide (no fixed storage)");
  }
  ESP_LOGCONFIG(TAG, "  Max directory entries: %zu", this->max_dir_entries_);
}

namespace {
struct ListDirCtx {
  DirEntries *out;
};

bool list_dir_cb(const storage::FileStat *entry, void *ctx_ptr) {
  auto *ctx = static_cast<ListDirCtx *>(ctx_ptr);
  ctx->out->push_back(*entry);
  return true;
}
}  // namespace

storage::StorageError HttpFileApi::list_dir(const char *path, DirEntries &out, storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  out.init(this->max_dir_entries_);
  ListDirCtx ctx{&out};
  return s->list_dir(path, list_dir_cb, &ctx);
}

storage::StorageError HttpFileApi::stat(const char *path, storage::FileStat *out, storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  return s->stat(path, out);
}

storage::StorageError HttpFileApi::read_file(const char *path, storage::RamBuffer &out, size_t *size,
                                             storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  return storage::read_file(s, path, out, size);
}

storage::StorageError HttpFileApi::write_file(const char *path, const uint8_t *data, size_t size,
                                              storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  auto err = storage::write_file(s, path, data, size);
  if (err == storage::StorageError::OK)
    this->on_upload_complete_.call(path);
  return err;
}

storage::StorageError HttpFileApi::remove(const char *path, bool recursive, storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  auto err = recursive ? storage::remove_recursive(s, path) : s->remove(path);
  if (err == storage::StorageError::OK)
    this->on_delete_.call(path);
  return err;
}

storage::StorageError HttpFileApi::mkdir(const char *path, storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  auto err = s->mkdir(path);
  if (err == storage::StorageError::OK)
    this->on_mkdir_.call(path);
  return err;
}

storage::StorageError HttpFileApi::rename(const char *old_path, const char *new_path, storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  auto err = s->rename(old_path, new_path);
  if (err == storage::StorageError::OK)
    this->on_rename_.call(old_path, new_path);
  return err;
}

storage::StorageError HttpFileApi::get_storage_info(storage::StorageInfo *out, storage::PathStorage *storage) {
  auto *s = this->resolve_storage_(storage);
  if (s == nullptr)
    return storage::StorageError::NOT_READY;
  return s->get_info(out);
}

storage::StorageError HttpFileApi::copy(const char *src_path, const char *dst_path, storage::PathStorage *src_storage,
                                        storage::PathStorage *dst_storage) {
  auto *src = this->resolve_storage_(src_storage);
  auto *dst = this->resolve_storage_(dst_storage);
  if (src == nullptr || dst == nullptr)
    return storage::StorageError::NOT_READY;

  if (storage::global_storage_worker != nullptr) {
    return storage::global_storage_worker->async_copy(src, src_path, dst, dst_path,
                                                       [this](storage::StorageError err) {
                                                         this->on_copy_complete_.call(err);
                                                       });
  }

  // No async worker compiled in (no path-based driver requested it) — fall through to the
  // synchronous, BLOCKING helper. Same call signature and callback contract either way.
  auto err = storage::copy(src, src_path, dst, dst_path);
  this->on_copy_complete_.call(err);
  return err;
}

storage::StorageError HttpFileApi::move(const char *src_path, const char *dst_path, storage::PathStorage *src_storage,
                                        storage::PathStorage *dst_storage) {
  auto *src = this->resolve_storage_(src_storage);
  auto *dst = this->resolve_storage_(dst_storage);
  if (src == nullptr || dst == nullptr)
    return storage::StorageError::NOT_READY;

  if (storage::global_storage_worker != nullptr) {
    return storage::global_storage_worker->async_move(src, src_path, dst, dst_path,
                                                       [this](storage::StorageError err) {
                                                         this->on_move_complete_.call(err);
                                                       });
  }

  auto err = storage::move(src, src_path, dst, dst_path);
  this->on_move_complete_.call(err);
  return err;
}

}  // namespace esphome::http_file_api
