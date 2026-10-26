#pragma once

#include "esphome/core/defines.h"

#if defined(USE_STORAGE_ISO_MOUNT) && defined(USE_STORAGE_WORKER)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "iso9660.h"
#include "storage.h"
#include "storage_worker.h"

#include <functional>
#include <string>

namespace esphome::storage::iso9660 {

class IsoMounter;

class IsoImageStorage : public OpticalStorage {
 public:
  using DoneCallback = std::function<void(StorageError)>;
  static constexpr size_t MOUNT_PATH_SIZE = 32;
  static constexpr uint32_t READ_SECTORS = 32;

  explicit IsoImageStorage(IsoMounter *owner) : owner_(owner) {}

  StorageError begin(const char *mount_path, const char *image_vfs, DoneCallback &&done);
  bool is_ready() const { return this->state_ == State::MOUNTED; }
  bool is_mounting() const { return this->state_ == State::MOUNTING; }
  bool names(const char *path) const;
  void detach();
  void release();
  const char *image_path() const { return this->image_; }
  const Index &index() const { return this->index_; }

  StorageError get_info(StorageInfo *info) override;
  uint8_t get_capabilities() const override;
  const Storage *get_backing_storage() const override { return this->backing_; }
  uint8_t get_mount_caps() const override { return MountableStorage::MOUNT_CAP_UNMOUNT; }

  StorageError mount() override;
  StorageError unmount() override;
  StorageError stat(const char *path, FileStat *st) override;
  StorageError list_dir(const char *path, bool (*callback)(const FileStat *entry, void *ctx), void *ctx) override;
  bool is_emulated() const override { return true; }
  bool has_medium() const override { return this->state_ == State::MOUNTED; }
  StorageError get_extent(const char *path, OpticalExtent *out) override;
  StorageError get_medium(OpticalMedium *out) override;

 protected:
  enum class State : uint8_t { IDLE, MOUNTING, MOUNTED, DETACHED };

  bool borrow_buffer_();
  void release_buffer_();
  void step_();
  void on_open_(StorageError err);
  void on_seek_(StorageError err);
  void on_read_(StorageError err);
  void close_(StorageError err);
  void finish_(StorageError err);

  IsoMounter *owner_;
  PathStorage *backing_{nullptr};
  const char *rel_{nullptr};
  State state_{State::IDLE};
  StorageError result_{StorageError::STORAGE_ERROR_OK};
  StreamHandle stream_{};
  uint8_t *buf_{nullptr};
  uint32_t buf_sectors_{0};
  bool buf_borrowed_{false};
  size_t want_{0};
  size_t got_{0};
  DoneCallback done_;
  Builder builder_;
  Index index_;
  char mount_buf_[MOUNT_PATH_SIZE]{};
  char image_[STORAGE_VFS_PATH_MAX]{};
};

class IsoMounter : public Component {
 public:
  static constexpr uint8_t MAX_IMAGES = 9;

  void set_mount_path(const char *base) { this->base_ = base; }
  void set_max_images(uint8_t count) { this->max_ = count > MAX_IMAGES ? MAX_IMAGES : count; }
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  StorageError mount(const char *image_vfs, IsoImageStorage::DoneCallback &&done);
  StorageError unmount(const char *path);
  StorageError unmount(IsoImageStorage *image);

 protected:
  friend class IsoImageStorage;

  void finished_(IsoImageStorage *image, StorageError err);
  void on_quiesce_(Storage *s);
  int slot_of_(const IsoImageStorage *image) const;
  void destroy_later_(IsoImageStorage *image);

  const char *base_{"/iso"};
  uint8_t max_{1};
  IsoImageStorage *slots_[MAX_IMAGES]{};
};

extern IsoMounter *global_iso_mounter;

inline void iso_fire(Trigger<std::string> *trigger, StorageError err) {
  trigger->trigger(err == StorageError::STORAGE_ERROR_OK ? std::string() : std::string(error_to_string(err)));
}

template<typename... Ts> class IsoMountAction : public Action<Ts...> {
 public:
  explicit IsoMountAction(IsoMounter *mounter) : mounter_(mounter) {}
  TEMPLATABLE_VALUE(std::string, file)
  Trigger<std::string> *get_complete_trigger() { return &this->complete_trigger_; }

  void play(const Ts &...x) override {
    const std::string file = this->file_.value(x...);
    Trigger<std::string> *trigger = &this->complete_trigger_;
    const StorageError err = this->mounter_->mount(file.c_str(), [trigger](StorageError e) { iso_fire(trigger, e); });
    if (err != StorageError::STORAGE_ERROR_OK)
      iso_fire(trigger, err);
  }

 protected:
  IsoMounter *mounter_;
  Trigger<std::string> complete_trigger_;
};

template<typename... Ts> class IsoUnmountAction : public Action<Ts...> {
 public:
  explicit IsoUnmountAction(IsoMounter *mounter) : mounter_(mounter) {}
  TEMPLATABLE_VALUE(std::string, path)
  Trigger<std::string> *get_complete_trigger() { return &this->complete_trigger_; }

  void play(const Ts &...x) override {
    const std::string path = this->path_.value(x...);
    iso_fire(&this->complete_trigger_, this->mounter_->unmount(path.c_str()));
  }

 protected:
  IsoMounter *mounter_;
  Trigger<std::string> complete_trigger_;
};

}

#endif
