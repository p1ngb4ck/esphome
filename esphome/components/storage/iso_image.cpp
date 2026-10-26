#include "iso_image.h"

#if defined(USE_STORAGE_ISO_MOUNT) && defined(USE_STORAGE_WORKER)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "transfer_buffer.h"

#include <cstring>
#include <new>

namespace esphome::storage::iso9660 {

static const char *const TAG = "storage.iso";

IsoMounter *global_iso_mounter = nullptr;

StorageError IsoImageStorage::begin(const char *mount_path, const char *image_vfs, DoneCallback &&done) {
  if (global_storage_registry == nullptr || global_storage_worker == nullptr)
    return StorageError::STORAGE_ERROR_NOT_READY;
  const size_t image_len = strlen(image_vfs);
  const size_t mount_len = strlen(mount_path);
  if (image_len >= sizeof(this->image_) || mount_len >= sizeof(this->mount_buf_))
    return StorageError::STORAGE_ERROR_INVALID_ARGS;
  memcpy(this->image_, image_vfs, image_len + 1);
  const char *rel = nullptr;
  PathStorage *ps = global_storage_registry->resolve_path(this->image_, &rel);
  if (ps == nullptr || rel == nullptr || rel[0] == '\0')
    return StorageError::STORAGE_ERROR_NOT_FOUND;
  if (ps->get_backing_storage() != nullptr)
    return StorageError::STORAGE_ERROR_NOT_SUPPORTED;
  memcpy(this->mount_buf_, mount_path, mount_len + 1);
  this->set_mount_path_(this->mount_buf_);
  if (!this->borrow_buffer_())
    return StorageError::STORAGE_ERROR_NO_SPACE;
  this->backing_ = ps;
  this->rel_ = rel;
  this->builder_.start(&this->index_, 0, this->buf_sectors_);
  const StorageError err =
      global_storage_worker->begin_read(ps, rel, &this->stream_, [this](StorageError e) { this->on_open_(e); });
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->release_buffer_();
    this->backing_ = nullptr;
    this->rel_ = nullptr;
    return err;
  }
  this->done_ = std::move(done);
  this->state_ = State::MOUNTING;
  return StorageError::STORAGE_ERROR_OK;
}

bool IsoImageStorage::borrow_buffer_() {
#if defined(USE_STORAGE_TRANSFER_BUFFER) && defined(USE_ESP32)
  if (global_transfer_buffer != nullptr) {
    this->buf_ = global_transfer_buffer->try_acquire(READ_SECTORS * SECTOR_SIZE);
    if (this->buf_ != nullptr) {
      this->buf_borrowed_ = true;
      this->buf_sectors_ = READ_SECTORS;
      return true;
    }
  }
#endif
  this->buf_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(SECTOR_SIZE);
  this->buf_borrowed_ = false;
  this->buf_sectors_ = 1;
  return this->buf_ != nullptr;
}

void IsoImageStorage::release_buffer_() {
  if (this->buf_ == nullptr)
    return;
#if defined(USE_STORAGE_TRANSFER_BUFFER) && defined(USE_ESP32)
  if (this->buf_borrowed_) {
    global_transfer_buffer->release();
    this->buf_ = nullptr;
    this->buf_borrowed_ = false;
    return;
  }
#endif
  RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->buf_, SECTOR_SIZE);
  this->buf_ = nullptr;
}

void IsoImageStorage::step_() {
  uint32_t lba = 0;
  uint32_t count = 0;
  switch (this->builder_.next(&lba, &count)) {
    case Builder::Step::READ:
      break;
    case Builder::Step::DONE:
      this->close_(StorageError::STORAGE_ERROR_OK);
      return;
    default:
      this->close_(this->builder_.error());
      return;
  }
  if (this->state_ != State::MOUNTING) {
    this->close_(StorageError::STORAGE_ERROR_NOT_READY);
    return;
  }
  this->want_ = static_cast<size_t>(count) * SECTOR_SIZE;
  this->got_ = 0;
  const StorageError err =
      global_storage_worker->seek(this->stream_, static_cast<int64_t>(lba) * SECTOR_SIZE, SeekMode::SEEK_MODE_SET,
                                  [this](StorageError e) { this->on_seek_(e); });
  if (err != StorageError::STORAGE_ERROR_OK)
    this->close_(err);
}

void IsoImageStorage::on_open_(StorageError err) {
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->finish_(err);
    return;
  }
  this->step_();
}

void IsoImageStorage::on_seek_(StorageError err) {
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->close_(err);
    return;
  }
  const StorageError e2 = global_storage_worker->read_chunk(this->stream_, this->buf_, this->want_, &this->got_,
                                                            [this](StorageError e) { this->on_read_(e); });
  if (e2 != StorageError::STORAGE_ERROR_OK)
    this->close_(e2);
}

void IsoImageStorage::on_read_(StorageError err) {
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->close_(err);
    return;
  }
  if (this->got_ != this->want_) {
    this->close_(StorageError::STORAGE_ERROR_CORRUPT);
    return;
  }
  this->builder_.feed(this->buf_, static_cast<uint32_t>(this->want_ / SECTOR_SIZE));
  this->step_();
}

void IsoImageStorage::close_(StorageError err) {
  this->result_ = err;
  const StorageError e2 =
      global_storage_worker->end_read(this->stream_, [this](StorageError) { this->finish_(this->result_); });
  if (e2 != StorageError::STORAGE_ERROR_OK)
    this->finish_(err);
}

void IsoImageStorage::finish_(StorageError err) {
  this->release_buffer_();
  if (err == StorageError::STORAGE_ERROR_OK &&
      (this->state_ != State::MOUNTING || this->backing_ == nullptr || this->index_.empty()))
    err = StorageError::STORAGE_ERROR_NOT_READY;
  if (err == StorageError::STORAGE_ERROR_OK) {
    this->state_ = State::MOUNTED;
    err = global_storage_registry->register_storage(this);
  }
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->state_ = State::IDLE;
    this->index_.clear();
    this->backing_ = nullptr;
  }
  DoneCallback done = std::move(this->done_);
  this->done_ = nullptr;
  this->owner_->finished_(this, err);
  if (done)
    done(err);
}

bool IsoImageStorage::names(const char *path) const {
  return strcmp(this->image_, path) == 0 || strcmp(this->mount_buf_, path) == 0;
}

void IsoImageStorage::detach() {
  if (this->state_ == State::MOUNTED)
    this->state_ = State::DETACHED;
  this->backing_ = nullptr;
}

void IsoImageStorage::release() {
  this->state_ = State::IDLE;
  this->index_.clear();
  this->backing_ = nullptr;
  this->rel_ = nullptr;
}

StorageError IsoImageStorage::get_info(StorageInfo *info) {
  info->id = this->mount_buf_;
  info->name = this->index_.label()[0] != '\0' ? this->index_.label() : "ISO image";
  info->kind = "iso";
  info->total_bytes = this->state_ == State::MOUNTED ? this->index_.volume_bytes() : 0;
  info->free_bytes = 0;
  info->block_size = SECTOR_SIZE;
  info->is_mounted = this->state_ == State::MOUNTED;
  info->is_removable = true;
  info->is_read_only = true;
  return StorageError::STORAGE_ERROR_OK;
}

uint8_t IsoImageStorage::get_capabilities() const {
  if (this->backing_ == nullptr)
    return 0;
  return this->backing_->get_capabilities() & StorageCaps::STORAGE_CAP_IO_TASK_SAFE;
}

StorageError IsoImageStorage::mount() {
  return this->state_ == State::MOUNTED ? StorageError::STORAGE_ERROR_OK : StorageError::STORAGE_ERROR_NOT_SUPPORTED;
}

StorageError IsoImageStorage::unmount() { return this->owner_->unmount(this); }

StorageError IsoImageStorage::open(const char *path, FileHandle *&handle, OpenMode mode) {
  if (mode != OpenMode::OPEN_MODE_READ)
    return StorageError::STORAGE_ERROR_PERMISSION_DENIED;
  return this->state_ == State::MOUNTED ? StorageError::STORAGE_ERROR_NOT_SUPPORTED
                                        : StorageError::STORAGE_ERROR_NOT_READY;
}

StorageError IsoImageStorage::stat(const char *path, FileStat *st) {
  if (this->state_ != State::MOUNTED)
    return StorageError::STORAGE_ERROR_NOT_READY;
  return this->index_.stat(path, st);
}

StorageError IsoImageStorage::list_dir(const char *path, bool (*callback)(const FileStat *entry, void *ctx),
                                       void *ctx) {
  if (this->state_ != State::MOUNTED)
    return StorageError::STORAGE_ERROR_NOT_READY;
  return this->index_.list(path, callback, ctx);
}

StorageError IsoImageStorage::get_file_window(const char *path, FileWindow *out) {
  if (this->state_ != State::MOUNTED || this->backing_ == nullptr)
    return StorageError::STORAGE_ERROR_NOT_READY;
  Entry e{};
  const StorageError err = this->index_.lookup(path, &e);
  if (err != StorageError::STORAGE_ERROR_OK)
    return err;
  if (e.is_dir)
    return StorageError::STORAGE_ERROR_INVALID_ARGS;
  if (!e.contiguous)
    return StorageError::STORAGE_ERROR_NOT_SUPPORTED;
  out->storage = this->backing_;
  out->path = this->rel_;
  out->offset = static_cast<uint64_t>(e.lba) * SECTOR_SIZE;
  out->length = e.size;
  return StorageError::STORAGE_ERROR_OK;
}

void IsoMounter::setup() {
  if (global_storage_registry == nullptr) {
    this->mark_failed();
    return;
  }
  global_storage_registry->add_on_quiesce_callback([this](Storage *s) { this->on_quiesce_(s); });
  global_iso_mounter = this;
}

void IsoMounter::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ISO mount:\n"
                "  Mount path: %s\n"
                "  Images: %u",
                this->base_, this->max_);
}

int IsoMounter::slot_of_(const IsoImageStorage *image) const {
  for (uint8_t i = 0; i < this->max_; i++) {
    if (this->slots_[i] == image)
      return i;
  }
  return -1;
}

StorageError IsoMounter::mount(const char *image_vfs, IsoImageStorage::DoneCallback &&done) {
  if (image_vfs == nullptr || image_vfs[0] != '/')
    return StorageError::STORAGE_ERROR_INVALID_ARGS;
  int free_slot = -1;
  for (uint8_t i = 0; i < this->max_; i++) {
    if (this->slots_[i] == nullptr) {
      if (free_slot < 0)
        free_slot = i;
    } else if (strcmp(this->slots_[i]->image_path(), image_vfs) == 0) {
      return StorageError::STORAGE_ERROR_ALREADY_EXISTS;
    }
  }
  if (free_slot < 0)
    return StorageError::STORAGE_ERROR_NO_SPACE;
  char path[IsoImageStorage::MOUNT_PATH_SIZE];
  if (free_slot == 0) {
    snprintf(path, sizeof(path), "%s", this->base_);
  } else {
    snprintf(path, sizeof(path), "%s%d", this->base_, free_slot + 1);
  }
  RAMAllocator<IsoImageStorage> alloc(RAMAllocator<IsoImageStorage>::ALLOC_EXTERNAL);
  IsoImageStorage *raw = alloc.allocate(1);
  if (raw == nullptr)
    return StorageError::STORAGE_ERROR_NO_SPACE;
  auto *image = new (raw) IsoImageStorage(this);
  this->slots_[free_slot] = image;
  const StorageError err = image->begin(path, image_vfs, std::move(done));
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->slots_[free_slot] = nullptr;
    image->~IsoImageStorage();
    alloc.deallocate(image, 1);
    return err;
  }
  ESP_LOGD(TAG, "Mounting '%s' at '%s'", image_vfs, path);
  return StorageError::STORAGE_ERROR_OK;
}

void IsoMounter::finished_(IsoImageStorage *image, StorageError err) {
  if (err != StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "'%s' not mounted: %s", image->image_path(), error_to_string(err));
    const int slot = this->slot_of_(image);
    if (slot >= 0)
      this->slots_[slot] = nullptr;
    this->destroy_later_(image);
    return;
  }
  ESP_LOGI(TAG, "'%s' mounted at '%s' ('%s', %u index bytes)", image->image_path(), image->get_mount_path(),
           image->index().label(), static_cast<unsigned>(image->index().bytes()));
#ifdef USE_STORAGE_CHANGE_FEED
  global_storage_registry->note_dir_changed("");
#endif
}

StorageError IsoMounter::unmount(const char *path) {
  if (path == nullptr)
    return StorageError::STORAGE_ERROR_INVALID_ARGS;
  for (uint8_t i = 0; i < this->max_; i++) {
    if (this->slots_[i] != nullptr && this->slots_[i]->names(path))
      return this->unmount(this->slots_[i]);
  }
  return StorageError::STORAGE_ERROR_NOT_FOUND;
}

StorageError IsoMounter::unmount(IsoImageStorage *image) {
  const int slot = this->slot_of_(image);
  if (slot < 0)
    return StorageError::STORAGE_ERROR_NOT_FOUND;
  if (image->is_mounting())
    return StorageError::STORAGE_ERROR_NOT_READY;
  global_storage_registry->unregister_storage(image);
  image->release();
  this->slots_[slot] = nullptr;
#ifdef USE_STORAGE_CHANGE_FEED
  global_storage_registry->note_dir_changed("");
#endif
  ESP_LOGI(TAG, "'%s' unmounted from '%s'", image->image_path(), image->get_mount_path());
  this->destroy_later_(image);
  return StorageError::STORAGE_ERROR_OK;
}

void IsoMounter::on_quiesce_(Storage *s) {
  for (uint8_t i = 0; i < this->max_; i++) {
    IsoImageStorage *image = this->slots_[i];
    if (image == nullptr || image == s || image->get_backing_storage() != s)
      continue;
    image->detach();
    if (image->is_mounting())
      continue;
    this->defer([this, image]() {
      if (this->slot_of_(image) >= 0)
        this->unmount(image);
    });
  }
}

void IsoMounter::destroy_later_(IsoImageStorage *image) {
  this->defer([image]() {
    image->~IsoImageStorage();
    RAMAllocator<IsoImageStorage>(RAMAllocator<IsoImageStorage>::ALLOC_EXTERNAL).deallocate(image, 1);
  });
}

}

#endif
