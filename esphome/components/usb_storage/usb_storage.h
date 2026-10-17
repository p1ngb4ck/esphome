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

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageClient
//
// Single USBClient subclass that owns the full MSC Bulk-Only Transport stack.
//
// Lifecycle:
//   setup()           -- register FATFS DISKIO driver, create transfer semaphore
//   on_connected()    -- parse bulk endpoints, run SCSI INQUIRY, wait for the unit to be
//                       ready (TEST UNIT READY / REQUEST SENSE), then READ CAPACITY,
//                       mount FAT filesystem, notify USBStorageDevice instances
//   on_disconnected() -- unmount filesystem, reset drive slot
// ─────────────────────────────────────────────────────────────────────────────
class USBStorageClient : public usb_host::USBClient {
 public:
#ifdef USE_STORAGE_FILE_SYSTEM_SELECT
  // file_system option (only exists with esp32 enable_exfat): 0 auto, 1 fat32, 2 exfat --
  // values from storage/__init__.py file_system_to_code().
  void set_requested_file_system(uint8_t fs) { this->requested_file_system_ = fs; }
#endif
  // Tear down the FAT mount + VFS registration of one slot without touching the USB level.
  // Used by USBStorageDevice::unmount_device() (safe eject): after this the mount
  // path is gone from the VFS entirely; the slot mounts again once its medium was out and
  // back in, the device was (re)inserted, or check_card() asks for it.
  void unmount_filesystem(uint8_t index);
  /// Mounts a slot that was unmounted by request again, once its medium answers. False: no device.
  bool mount_filesystem(uint8_t index);
  USBStorageClient() : usb_host::USBClient(0, 0) { this->add_class_match(USB_CLASS_MASS_STORAGE); }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  void add_device(USBStorageDevice *device) { this->devices_.push_back(device); }
  /// Another slot of a card reader (LUN), mounted by its own device entry.
  void add_lun(uint8_t lun, USBStorageDevice *device) { this->extra_luns_.push_back({lun, device}); }
  /// Medium check while connected, ms; SCHEDULER_DONT_RUN: only on connect and check_card().
  void set_card_check_interval(uint32_t ms) { this->card_check_interval_ = ms; }
  /// Looks at every slot now: mounts a medium that came, unmounts one that went. Also mounts a
  /// slot that was ejected.
  void check_card();

  // Called from DISKIO driver (FATFS context) -- blocking SCSI transfers. index: the slot (LUN entry).
  bool scsi_read(uint8_t index, uint32_t lba, uint8_t *buf, uint32_t count);
  bool scsi_write(uint8_t index, uint32_t lba, const uint8_t *buf, uint32_t count);
  uint32_t get_sector_count(uint8_t index) const { return this->luns_[index].sector_count; }
  uint32_t get_sector_size(uint8_t index) const { return this->luns_[index].sector_size; }
  bool is_disk_ready(uint8_t index) const { return this->luns_[index].ready; }
  void set_format_on_mismatch(bool format_on_mismatch) { this->format_on_mismatch_ = format_on_mismatch; }

 protected:
  void on_connected() override;
  // Connected once the reader answered INQUIRY; its slots are mounted when a medium is in.
  bool reports_connection_itself() const override { return true; }
  void on_disconnected() override;
  void on_removed(usb_device_handle_t handle) override;
  usb_host::MatchTier refine_match_(const usb_host::RoutedDevice &dev, usb_host::MatchTier tier) const override;

  // One slot of the device: a stick has LUN 0 only, a card reader one per slot.
  struct Lun {
    uint8_t number{0};
    USBStorageDevice *device{nullptr};
    const char *mount_path{nullptr};
    int drive{-1};
    uint32_t sector_count{0};
    uint32_t sector_size{512};
    bool ready{false};    // the FATFS drive may do I/O
    bool mounted{false};
    bool ejected{false};  // unmounted by request, medium still in
    uint8_t tries{0};
  };
  struct ExtraLun {
    uint8_t number;
    USBStorageDevice *device;
  };

  // The medium check: TEST UNIT READY, REQUEST SENSE, READ CAPACITY of each slot in turn, each a
  // command whose transfers are chained in their callbacks (USB task), so the main loop never
  // waits for the device.
  enum class Step : uint8_t { TUR, SENSE, CAPACITY };
  bool async_command_(Step step, const uint8_t *cdb, uint8_t cdb_len, uint8_t data_len);
  void async_csw_();
  void async_finish_(uint8_t status);
  void async_done_(Step step, uint8_t status);
  void async_reset_();
  void start_check_();
  void check_lun_(uint8_t index);
  void next_lun_();
  bool mount_lun_(uint8_t index);
  void unmount_lun_(uint8_t index);
  bool get_max_lun_(uint8_t &max_lun);

  bool alloc_data_();
  void free_data_();
  bool scsi_read_(uint8_t lun, uint32_t lba, uint8_t *buf, uint32_t count, uint32_t sector_size);
  bool scsi_write_(uint8_t lun, uint32_t lba, const uint8_t *buf, uint32_t count, uint32_t sector_size);
  bool parse_msc_endpoints_();
  // Bulk IN transfers must ask for a whole number of max-size packets. The BOT/SCSI
  // responses are 8, 13 or 36 bytes, none of which is a multiple of any legal
  // wMaxPacketSize, so every IN request is rounded up here. The device answers with a
  // short packet and TransferStatus::data_len reports what actually arrived.
  uint16_t round_up_to_in_mps_(uint16_t len) const;

  bool scsi_inquiry_();
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

  bool send_cbw_(uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t *cdb, uint8_t cdb_len, uint8_t lun = 0);
  // check_condition_expected: a CHECK CONDITION status is an answer here (TEST UNIT READY), not logged
  bool recv_csw_(uint32_t expected_tag, bool check_condition_expected = false);

  bool wait_transfer_(uint32_t timeout_ms = 5000);
  static void transfer_done_cb(const usb_host::TransferStatus &status, USBStorageClient *client);

  std::vector<USBStorageDevice *> devices_{};
  std::vector<ExtraLun> extra_luns_{};
  std::vector<Lun> luns_{};

  uint8_t bulk_in_ep_{0};
  uint8_t bulk_out_ep_{0};
  // wMaxPacketSize of the bulk endpoints, taken from the descriptors rather than assumed.
  // A bulk IN transfer has to request a whole number of these, see round_up_to_in_mps_();
  // both directions use them to split a data phase into whole packets.
  uint16_t bulk_in_mps_{0};
  uint16_t bulk_out_mps_{0};
  uint8_t msc_interface_{0xFF};

  uint32_t cbw_tag_{1};
  uint8_t max_lun_{0};

  bool format_on_mismatch_{false};
#ifdef USE_STORAGE_FILE_SYSTEM_SELECT
  uint8_t requested_file_system_{0};  // FS_SELECT_AUTO
#endif

  uint32_t card_check_interval_{10000};
  uint32_t connection_{0};  // counts connects and disconnects: an answer of an earlier one is dropped
  bool checking_{false};
  bool check_again_{false};
  uint8_t check_index_{0};
  // The command in flight (USB task while it runs, main loop after)
  Step async_step_{Step::TUR};
  uint32_t async_tag_{0};
  uint8_t async_len_{0};
  uint8_t async_buf_[18]{};

  SemaphoreHandle_t transfer_sem_{nullptr};
  bool transfer_ok_{false};
  bool transfer_stalled_{false};
  uint16_t transfer_len_{0};
  DataXfer data_[DATA_XFERS]{};
  // The bulk pipes and the data transfers: one command at a time, of the storage worker or the
  // medium check. A binary semaphore: the check gives it back from a transfer callback.
  SemaphoreHandle_t io_lock_{nullptr};
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
// User-facing device instance. Extends FilesystemStorage so it participates
// in the StorageRegistry. Mount/unmount are driven by USBStorageClient events;
// all file ops go through POSIX (FAT is already VFS-registered by the client).
// ─────────────────────────────────────────────────────────────────────────────
class USBStorageDevice : public esphome::storage::FilesystemStorage, public esphome::storage::MountableStorage {
  friend class USBStorageClient;

 public:
  static constexpr int MAX_OPEN_FILES = 4;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_client(USBStorageClient *client) { this->client_ = client; }
  void set_mount_path(const char *mount_path) { this->set_mount_path_(mount_path); }
  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }
  void set_storage_id(const char *id) { this->storage_id_ = id; }

  // Called by USBStorageClient when its slot's medium is mounted / gone
  void on_device_connected(const char *mount_path, int drive);
  void on_device_disconnected();
  void set_lun_index(uint8_t index) { this->lun_index_ = index; }

  template<typename F> void add_on_mounted_callback(F &&cb) { this->on_mounted_.add(std::forward<F>(cb)); }
  bool is_mounted() const { return this->fs_mounted_; }

  bool remount_device();
  void unmount_device();

  // storage::FilesystemStorage interface
  storage::StorageError get_info(storage::StorageInfo *info) override;
  storage::StorageError mount() override;
  storage::StorageError unmount() override;

  // No-RTTI downcast hook -- see PathStorage::as_mountable().
  storage::MountableStorage *as_mountable() override { return this; }
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

  USBFileHandle *alloc_handle_();
  void free_handle_(USBFileHandle *handle);
  void build_path_(char *out, size_t out_size, const char *path) const;

  // Builds a FATFS-native path ("N:/dir/file") from a path relative to this device's mount
  // point, using fatfs_drive_ (captured from the client at connect time -- see
  // on_device_connected()). Returns false if the result would exceed out_size.
  bool build_fatfs_path_(const char *rel_path, char *out, size_t out_size) const;

  void log_list_dir_start_(const char *path) const;
  // Matches the list_dir() callback signature (bool return = keep enumerating).
  static bool log_list_dir_entry(const storage::FileStat *entry, void *ctx);

  USBStorageClient *client_{nullptr};
  uint8_t lun_index_{0};  // the client's slot this device mounts
  uint16_t vid_{0};
  uint16_t pid_{0};
  const char *storage_id_{nullptr};
  bool fs_mounted_{false};
  // Deferred-unmount state: unmount() only requests the unmount (sets this flag); loop()
  // performs the actual sync + unmount once no file handle is open and no worker job still
  // touches this device. is_mounted() stays true until then, so an interval-based remount
  // check does not race the pending unmount.
  bool unmount_pending_{false};
  // mount() may run on the storage worker's task; loop() hands it to the client.
  std::atomic<bool> mount_pending_{false};
  // True while a worker job that references this device is in flight -- the worker cannot be
  // queried for "jobs touching storage X" cheaply, so the device tracks it via open handles
  // (every transfer holds a handle open for its duration) plus this guard for the brief
  // windows around open/close. Open-handle count is derived from handle_pool_.
  bool has_open_handles_() const;
  char fatfs_drive_[3]{};  // "N:" -- the slot's drive, given on connect

  USBFileHandle handle_pool_[MAX_OPEN_FILES]{};
  LazyCallbackManager<void(const char *)> on_mounted_;
};

}  // namespace esphome::usb_storage
#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
