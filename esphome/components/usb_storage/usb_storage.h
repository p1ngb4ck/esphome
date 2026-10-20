#pragma once

#include "esphome/components/storage/storage.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "msc_bot.h"

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include <vector>

namespace esphome::usb_storage {

static constexpr const char *TAG = "usb_storage";

class USBStorageDevice;
class USBStorageClient;
template<typename... Ts> class ListFilesAction;

/// An optional entry of the YAML: the mount path (and format_on_mismatch) for the stick it names (vid / pid) or for
/// any stick (0 / 0). A codegen table in flash; a stick without an entry gets the default mount path.
struct USBStorageEntry {
  uint16_t vid;
  uint16_t pid;
  const char *mount_path;
  bool format_on_mismatch;
};

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageClient
//
// One USBClient serving any number of sticks at once (accepts_more_devices_()). Nothing of a
// stick exists before it is plugged in: on_device_connected() makes its USBStorageDevice (MSC
// state, FATFS drive, data transfers, file handles), free_device_context() deletes it once the
// stick is gone and none of its transfers is out any more.
// ─────────────────────────────────────────────────────────────────────────────
class USBStorageClient : public usb_host::USBClient {
 public:
  USBStorageClient() : usb_host::USBClient(0, 0) { this->add_class_match(USB_CLASS_MASS_STORAGE); }

#ifdef USE_STORAGE_FILE_SYSTEM_SELECT
  // file_system option (only exists with esp32 enable_exfat): 0 auto, 1 fat32, 2 exfat --
  // values from storage/__init__.py file_system_to_code().
  void set_requested_file_system(uint8_t fs) { this->requested_file_system_ = fs; }
#endif
  void set_entries(const USBStorageEntry *entries, uint8_t count) {
    this->entries_ = entries;
    this->entry_count_ = count;
  }
  void set_max_devices(uint8_t max_devices) { this->max_devices_ = max_devices; }

  void loop() override;

  /// A stick is mounted (any, or the one at @p mount_path).
  bool is_mounted() const { return this->find_mounted(nullptr) != nullptr; }
  bool is_mounted(const char *mount_path) const { return this->find_mounted(mount_path) != nullptr; }
  /// The mounted stick at @p mount_path; nullptr or "" : the first mounted one.
  USBStorageDevice *find_mounted(const char *mount_path) const;

  /// on_mounted of an entry: (entry index, mount path) of a stick mounted by an entry.
  template<typename F> void add_on_mounted_callback(F &&cb) { this->on_mounted_.add(std::forward<F>(cb)); }

 protected:
  friend class USBStorageDevice;

  bool accepts_more_devices_() const override { return true; }
  void on_device_connected(usb_host::UsbDevice &dev) override;
  void on_device_disconnected(usb_host::UsbDevice &dev) override;
  void free_device_context(usb_host::UsbDevice &dev) override;
  usb_host::MatchTier refine_match_(const usb_host::RoutedDevice &dev, usb_host::MatchTier tier) const override;

  /// The entry naming this stick, else one for any stick, whose mount path is free (-1: none).
  int find_entry_(uint16_t vid, uint16_t pid) const;
  bool path_in_use_(const char *mount_path) const;

  const USBStorageEntry *entries_{nullptr};
  uint8_t entry_count_{0};
  uint8_t max_devices_{1};
#ifdef USE_STORAGE_FILE_SYSTEM_SELECT
  uint8_t requested_file_system_{0};  // FS_SELECT_AUTO
#endif
  std::vector<USBStorageDevice *> sticks_{};
  LazyCallbackManager<void(int, const char *)> on_mounted_;
};

// ─────────────────────────────────────────────────────────────────────────────
// USBFileHandle -- extends storage::FileHandle with no extra fields needed;
// the base FILE *file is sufficient since FAT is VFS-backed.
// ─────────────────────────────────────────────────────────────────────────────
struct USBFileHandle : public esphome::storage::FileHandle {
  char path_buf[(ESP_VFS_PATH_MAX + storage::STORAGE_PATH_MAX + 1)]{};
};

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageDevice
//
// One plugged stick: the MSC Bulk-Only Transport stack on its UsbDevice and the FilesystemStorage
// it is in the StorageRegistry as. Made by the client when the stick connects, deleted after it
// is gone; all file ops go through POSIX (FAT is VFS-registered on the stick's mount path).
//
//   start_()  -- semaphores, data transfers, FATFS drive, bulk endpoints, claim, SCSI INQUIRY,
//                then check_ready_() (TEST UNIT READY / REQUEST SENSE) and finish_connect_()
//                (READ CAPACITY, mount, StorageRegistry)
//   stop_()   -- StorageRegistry, unmount, transfers back, interface released
// ─────────────────────────────────────────────────────────────────────────────
class USBStorageDevice : public esphome::storage::FilesystemStorage, public esphome::storage::MountableStorage {
  friend class USBStorageClient;

 public:
  static constexpr int MAX_OPEN_FILES = 4;

  USBStorageDevice(USBStorageClient *client, usb_host::UsbDevice *dev) : client_(client), dev_(dev) {}
  ~USBStorageDevice() override;

  bool is_mounted() const { return this->fs_mounted_; }
  bool remount_device();
  void unmount_device();

  // Called from DISKIO driver (FATFS context) -- blocking SCSI transfers
  bool scsi_read(uint32_t lba, uint8_t *buf, uint32_t count);
  bool scsi_write(uint32_t lba, const uint8_t *buf, uint32_t count);
  uint32_t get_sector_count() const { return this->sector_count_; }
  uint32_t get_sector_size() const { return this->sector_size_; }
  bool is_disk_ready() const { return this->disk_ready_; }

  // storage::FilesystemStorage interface
  storage::StorageError get_info(storage::StorageInfo *info) override;
  storage::StorageError mount() override;
  storage::StorageError unmount() override;

  // No-RTTI downcast hook -- see PathStorage::as_mountable().
  storage::MountableStorage *as_mountable() override { return this; }
  // The FAT mount lifecycle is owned by the USB hotplug client -- insertion auto-mounts, so
  // only the safe-eject direction may be invoked externally (mount() below is a status no-op).
  uint8_t get_mount_caps() const override { return storage::MountableStorage::MOUNT_CAP_UNMOUNT; }
  storage::StorageError format() override;
  storage::StorageError sync() override;
  storage::StorageError open(const char *path, storage::FileHandle *&handle, storage::OpenMode mode) override;
  storage::StorageError close(storage::FileHandle *handle) override;
  storage::StorageError read(storage::FileHandle *handle, uint8_t *buf, size_t len, size_t *bytes_transferred) override;
  storage::StorageError write(storage::FileHandle *handle, const uint8_t *buf, size_t len,
                              size_t *bytes_transferred) override;
  storage::StorageError seek(storage::FileHandle *handle, int64_t offset, storage::SeekMode mode) override;
  storage::StorageError tell(storage::FileHandle *handle, uint64_t *position) override;
  storage::StorageError stat(const char *path, storage::FileStat *stat) override;
  storage::StorageError list_dir(const char *path, bool (*callback)(const storage::FileStat *entry, void *ctx),
                                 void *ctx) override;
  storage::StorageError mkdir(const char *path) override;
  storage::StorageError rmdir(const char *path) override;
  storage::StorageError remove(const char *path) override;
  storage::StorageError rename(const char *old_path, const char *new_path) override;

  // USB stack transfers are self-contained (own endpoint transfers via a FreeRTOS semaphore,
  // no shared bus with other main-loop-driven components) -- safe for the async worker to drive
  // from a background task, same as SdMmc's dedicated SDIO controller.
  uint8_t get_capabilities() const override { return storage::StorageCaps::STORAGE_CAP_IO_TASK_SAFE; }

 protected:
  template<typename... Ts> friend class ListFilesAction;

  // Main loop, from the client
  void start_();
  void stop_();
  void check_ready_();
  void finish_connect_();
  void unmount_filesystem_();
  /// The deferred unmount, once nothing references the stick; false while it still waits.
  bool try_unmount_();

  bool alloc_data_();
  void free_data_();
  bool scsi_read_(uint32_t lba, uint8_t *buf, uint32_t count);
  bool scsi_write_(uint32_t lba, const uint8_t *buf, uint32_t count);
  bool parse_msc_endpoints_();
  // Bulk IN transfers must ask for a whole number of max-size packets. The BOT/SCSI
  // responses are 8, 13 or 36 bytes, none of which is a multiple of any legal
  // wMaxPacketSize, so every IN request is rounded up here. The device answers with a
  // short packet and TransferStatus::data_len reports what actually arrived.
  uint16_t round_up_to_in_mps_(uint16_t len) const;

  bool scsi_inquiry_();
  bool scsi_read_capacity_();
  bool scsi_test_unit_ready_();
  bool scsi_request_sense_(uint8_t &key, uint8_t &asc);
  bool clear_halt_(uint8_t ep);
  bool reset_recovery_();

  // Data phase of READ(10)/WRITE(10) on two transfers of its own: one is on the bus while the
  // other is copied, each up to DATA_XFER_SIZE bytes. On the ESP32-P4 they are PSRAM
  // (CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM), elsewhere the USB DMA only reaches internal
  // RAM, so they stay one sector small there.
  static constexpr uint8_t DATA_XFERS = 2;
#ifdef USE_ESP32_VARIANT_ESP32P4
  static constexpr size_t DATA_XFER_SIZE = 32768;
#else
  static constexpr size_t DATA_XFER_SIZE = 512;
#endif
  struct DataXfer {
    usb_transfer_t *xfer{nullptr};
    SemaphoreHandle_t done{nullptr};
    uint32_t len{0};
    bool busy{false};
  };
  enum class DataResult : uint8_t { OK, STALL, FAIL };
  static void data_done_cb(usb_transfer_t *xfer);
  uint32_t data_piece_(uint16_t mps) const;
  bool data_submit_(DataXfer &d, uint8_t ep, uint32_t len);
  void data_cancel_(uint8_t ep);
  DataResult data_phase_(bool in, uint8_t *in_buf, const uint8_t *out_buf, uint32_t len);
  // Data phase and status of a command whose CBW went out; recovers the pipe on failure.
  bool data_command_(bool in, uint8_t *in_buf, const uint8_t *out_buf, uint32_t len, uint32_t tag);

  bool send_cbw_(uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t *cdb, uint8_t cdb_len);
  // check_condition_expected: a CHECK CONDITION status is an answer here (TEST UNIT READY), not logged
  bool recv_csw_(uint32_t expected_tag, bool check_condition_expected = false);

  bool wait_transfer_(uint32_t timeout_ms = 5000);
  static void transfer_done_cb(const usb_host::TransferStatus &status, USBStorageDevice *device);

  USBFileHandle *alloc_handle_();
  void free_handle_(USBFileHandle *handle);
  void build_path_(char *out, size_t out_size, const char *path) const;

  // Builds a FATFS-native path ("N:/dir/file") from a path relative to this device's mount
  // point, using fatfs_drive_str_. Returns false if the result would exceed out_size.
  bool build_fatfs_path_(const char *rel_path, char *out, size_t out_size) const;

  void log_list_dir_start_(const char *path) const;
  // Matches the list_dir() callback signature (bool return = keep enumerating).
  static bool log_list_dir_entry(const storage::FileStat *entry, void *ctx);

  // A file handle is open: the worker keeps one open for a transfer's whole duration.
  bool has_open_handles_() const;

  USBStorageClient *client_;
  usb_host::UsbDevice *dev_;
  int entry_{-1};  // the YAML entry whose mount path it has (-1: none, a default mount path)

  uint8_t bulk_in_ep_{0};
  uint8_t bulk_out_ep_{0};
  // wMaxPacketSize of the bulk endpoints, taken from the descriptors rather than assumed.
  // A bulk IN transfer has to request a whole number of these, see round_up_to_in_mps_();
  // both directions use them to split a data phase into whole packets.
  uint16_t bulk_in_mps_{0};
  uint16_t bulk_out_mps_{0};
  uint8_t msc_interface_{0xFF};

  uint32_t sector_count_{0};
  uint32_t sector_size_{512};
  uint32_t cbw_tag_{1};

  int fatfs_drive_{-1};
  char fatfs_drive_str_[4]{};  // "N:"
  bool disk_ready_{false};
  bool mounted_{false};     // FAT mounted and VFS-registered
  bool fs_mounted_{false};  // in the StorageRegistry
  bool format_on_mismatch_{false};
  // Deferred unmount: unmount() only requests it (any task); the client's loop syncs and unmounts
  // once no file handle is open and no worker job still touches this stick. is_mounted() stays
  // true until then, so an interval-based remount check does not race the pending unmount.
  std::atomic<bool> unmount_pending_{false};
  char mount_buf_[ESP_VFS_PATH_MAX + 1]{};

  SemaphoreHandle_t transfer_sem_{nullptr};
  bool transfer_ok_{false};
  bool transfer_stalled_{false};
  uint16_t transfer_len_{0};
  uint8_t ready_tries_{0};
  DataXfer data_[DATA_XFERS]{};
  SemaphoreHandle_t io_lock_{nullptr};  // SCSI commands of the storage worker against the transfers' lifetime

  USBFileHandle handle_pool_[MAX_OPEN_FILES]{};
};

}  // namespace esphome::usb_storage
#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
