#pragma once

#include "esphome/core/defines.h"

#if defined(USE_USB_CDROM) && defined(USE_STORAGE_ISO9660) && defined(USE_STORAGE_WORKER) && \
    (defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
     defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4))

#include "esphome/components/storage/iso9660.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/usb_storage/usb_storage.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <atomic>

namespace esphome::usb_cdrom {

static constexpr uint8_t SCSI_TYPE_CDROM = 0x05;

class USBCdrom;

struct CdromFileHandle : public storage::FileHandle {
  uint32_t lba{0};
  uint64_t size{0};
  uint64_t pos{0};
};

class CdromDrive : public storage::FilesystemStorage, public storage::MountableStorage {
 public:
  static constexpr int MAX_OPEN_FILES = 4;
  static constexpr size_t MOUNT_PATH_SIZE = 32;
  static constexpr uint32_t READ_SECTORS = 32;

  CdromDrive(USBCdrom *owner, usb_storage::USBStorageDevice *dev) : owner_(owner), dev_(dev) {}

  bool start(const char *mount_path);
  void stop();
  void check_media();
  storage::StorageError load();
  void on_mount_done(storage::StorageError err);
  bool media_mounted() const { return this->mounted_.load(std::memory_order_acquire); }
  bool reported_mounted() const { return this->reported_; }
  void set_reported(bool mounted) { this->reported_ = mounted; }
  usb_storage::USBStorageDevice *device() const { return this->dev_; }
  const char *label() const { return this->index_.label(); }

  storage::StorageError get_info(storage::StorageInfo *info) override;
  uint8_t get_capabilities() const override { return storage::StorageCaps::STORAGE_CAP_IO_TASK_SAFE; }
  storage::MountableStorage *as_mountable() override { return this; }
  uint8_t get_mount_caps() const override {
    return storage::MountableStorage::MOUNT_CAP_MOUNT | storage::MountableStorage::MOUNT_CAP_UNMOUNT;
  }

  storage::StorageError mount() override;
  storage::StorageError unmount() override;
  storage::StorageError sync() override { return storage::StorageError::STORAGE_ERROR_OK; }
  storage::StorageError open(const char *path, storage::FileHandle *&handle, storage::OpenMode mode) override;
  storage::StorageError close(storage::FileHandle *handle) override;
  storage::StorageError read(storage::FileHandle *handle, uint8_t *buf, size_t len, size_t *bytes_transferred) override;
  storage::StorageError write(storage::FileHandle *handle, const uint8_t *buf, size_t len,
                              size_t *bytes_transferred) override {
    return storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED;
  }
  storage::StorageError seek(storage::FileHandle *handle, int64_t offset, storage::SeekMode mode) override;
  storage::StorageError tell(storage::FileHandle *handle, uint64_t *position) override;
  storage::StorageError stat(const char *path, storage::FileStat *st) override;
  storage::StorageError list_dir(const char *path, bool (*callback)(const storage::FileStat *entry, void *ctx),
                                 void *ctx) override;
  storage::StorageError mkdir(const char *path) override { return storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED; }
  storage::StorageError rmdir(const char *path) override { return storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED; }
  storage::StorageError remove(const char *path) override {
    return storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED;
  }
  storage::StorageError rename(const char *old_path, const char *new_path) override {
    return storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED;
  }

 protected:
  enum class Presence : uint8_t { UNKNOWN, ABSENT, PRESENT, CHANGED, EJECT_REQUEST };

  bool lock_(bool wait);
  void unlock_();
  bool command_(const uint8_t *cdb, uint8_t cdb_len, uint8_t *in, uint16_t in_len);
  bool sense_(uint8_t &key, uint8_t &asc);
  Presence presence_();
  Presence unit_ready_();
  void medium_lock_(bool prevent);
  void start_stop_(bool load);
  uint16_t current_profile_();
  uint32_t last_session_();
  bool read_sectors_(uint32_t lba, uint8_t *buf, uint32_t count);
  storage::StorageError build_index_(uint32_t session);
  void release_media_();
  void request_mount_();

  USBCdrom *owner_;
  usb_storage::USBStorageDevice *dev_;
  storage::iso9660::Index index_;
  std::atomic<bool> mounted_{false};
  std::atomic<bool> check_mount_{false};
  bool reported_{false};
  bool registered_{false};
  bool events_supported_{true};
  bool locked_{false};
  bool mount_failed_{false};
  bool mount_pending_{false};
  bool present_{false};
  uint16_t profile_{0};
  uint8_t *sector_buf_{nullptr};
  uint32_t cached_lba_{UINT32_MAX};
  CdromFileHandle handles_[MAX_OPEN_FILES]{};
  char mount_buf_[MOUNT_PATH_SIZE]{};
};

class USBCdrom : public Component {
 public:
  static constexpr uint8_t MAX_DRIVES = 9;

  void set_mount_path(const char *base) { this->base_ = base; }
  void set_max_drives(uint8_t count) { this->max_ = count > MAX_DRIVES ? MAX_DRIVES : count; }
  void set_media_check_interval(uint32_t ms) { this->interval_ms_ = ms; }
  template<typename F> void add_on_media_mounted_callback(F &&cb) { this->on_mounted_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_media_removed_callback(F &&cb) { this->on_removed_.add(std::forward<F>(cb)); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  CdromDrive *attach(usb_storage::USBStorageDevice *dev);
  void detach(CdromDrive *drive);
  storage::StorageError eject(const char *mount_path);
  storage::StorageError load(const char *mount_path);
  void note_media_event() { this->enable_loop_soon_any_context(); }
  bool has_drive(const CdromDrive *drive) const;

 protected:
  CdromDrive *find_(const char *mount_path) const;
  void check_all_();

  const char *base_{"/cdrom"};
  uint8_t max_{1};
  uint32_t interval_ms_{2000};
  CdromDrive *drives_[MAX_DRIVES]{};
  LazyCallbackManager<void(const char *)> on_mounted_;
  LazyCallbackManager<void(const char *)> on_removed_;
};

template<typename... Ts> class EjectAction : public Action<Ts...> {
 public:
  EjectAction(USBCdrom *parent, const char *mount_path) : parent_(parent), mount_path_(mount_path) {}
  void play(const Ts &...x) override { this->parent_->eject(this->mount_path_); }

 protected:
  USBCdrom *parent_;
  const char *mount_path_;
};

template<typename... Ts> class LoadAction : public Action<Ts...> {
 public:
  LoadAction(USBCdrom *parent, const char *mount_path) : parent_(parent), mount_path_(mount_path) {}
  void play(const Ts &...x) override { this->parent_->load(this->mount_path_); }

 protected:
  USBCdrom *parent_;
  const char *mount_path_;
};

}

#endif
