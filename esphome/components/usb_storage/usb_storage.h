#pragma once

#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32P4)
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage_device.h"
#endif

#include "msc_bot.h"

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <functional>
#include <vector>
#include <string>

namespace esphome {
namespace usb_storage {

static constexpr const char *TAG = "usb_storage";
static constexpr uint8_t USB_CLASS_MASS_STORAGE = 0x08;

// Forward declarations
class USBStorageDevice;
class USBStorageClient;

using mount_ready_callback_t = std::function<void(const std::string &mount_path)>;

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageClient
//
// Single USBClient subclass that replaces the old MSCDetector + USBStorageHost
// split. Owns the full MSC Bulk-Only Transport stack — no msc_host lib needed.
//
// Lifecycle:
//   setup()        — register FATFS DISKIO driver, create transfer semaphore
//   on_connected() — parse bulk endpoints, run SCSI INQUIRY + READ CAPACITY,
//                    mount FAT filesystem, notify USBStorageDevice instances
//   on_disconnected() — unmount filesystem, reset drive slot
// ─────────────────────────────────────────────────────────────────────────────
class USBStorageClient : public usb_host::USBClient {
 public:
  USBStorageClient() : usb_host::USBClient(0, 0) {}

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }
  uint8_t get_interface_class() const override { return USB_CLASS_MASS_STORAGE; }

  void add_device(USBStorageDevice *device) { this->devices_.push_back(device); }

  // Called from DISKIO driver (FATFS context) — blocking SCSI transfers
  bool scsi_read(uint32_t lba, uint8_t *buf, uint32_t count);
  bool scsi_write(uint32_t lba, const uint8_t *buf, uint32_t count);
  uint32_t get_sector_count() const { return this->sector_count_; }
  uint32_t get_sector_size() const { return this->sector_size_; }
  bool is_disk_ready() const { return this->disk_ready_; }

 protected:
  void on_connected() override;
  void on_disconnected() override;
  void on_removed(usb_device_handle_t handle) override;

  // Endpoint discovery from config descriptor
  bool parse_msc_endpoints_();

  // SCSI helpers — all synchronous (block until USB callback fires)
  bool scsi_inquiry_();
  bool scsi_read_capacity_();
  bool scsi_test_unit_ready_();

  // BOT primitives
  bool send_cbw_(uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t *cdb, uint8_t cdb_len);
  bool recv_data_(uint8_t *buf, uint16_t len);
  bool send_data_(const uint8_t *buf, uint16_t len);
  bool recv_csw_(uint32_t expected_tag);

  // Transfer synchronisation — semaphore posted by USB-task callback, taken by caller
  bool wait_transfer_(uint32_t timeout_ms = 5000);

  // VID/PID dispatch to USBStorageDevice instances
  void notify_connected_(uint16_t vid, uint16_t pid);
  void notify_disconnected_();

  std::vector<USBStorageDevice *> devices_{};

  uint8_t bulk_in_ep_{0};
  uint8_t bulk_out_ep_{0};
  uint8_t msc_interface_{0};

  uint32_t sector_count_{0};
  uint32_t sector_size_{512};
  uint32_t cbw_tag_{1};

  int fatfs_drive_{-1};           // FATFS drive number assigned by usb_diskio_register()
  bool disk_ready_{false};
  bool mounted_{false};
  std::string mount_path_;        // set from matching USBStorageDevice on connect

  SemaphoreHandle_t transfer_sem_{nullptr};
  bool transfer_ok_{false};
  uint16_t transfer_len_{0};      // actual bytes transferred (for IN transfers)
};

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageDevice
//
// User-facing device instance. Holds mount path, VID/PID filter, automations,
// and StorageDevice interface. Parent pointer changed to USBStorageClient.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef USE_STORAGE
class USBStorageDevice : public Component, public Parented<USBStorageClient>, public storage::StorageDevice {
#else
class USBStorageDevice : public Component, public Parented<USBStorageClient> {
#endif
  friend class USBStorageClient;

 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_mount_path(const std::string &mount_path) { this->mount_path_ = mount_path; }
  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }
  void set_id(const std::string &id) { this->id_ = id; }

  // Called by USBStorageClient on connect/disconnect
  void on_device_connected(const std::string &mount_path);
  void on_device_disconnected();

  void list_files();

  void add_mount_ready_callback(const mount_ready_callback_t &callback) {
    this->mount_ready_callbacks_.push_back(callback);
  }
  const std::string &get_mount_path() const { return this->mount_path_; }
  bool is_mounted() const { return this->mounted_; }

  bool remount_device();
  void unmount_device();

#ifdef USE_STORAGE
  storage::StorageInfo get_info() override;
  bool is_available() override { return this->mounted_; }
  bool supports_filesystem() override { return true; }
  std::string get_mount_path() override { return this->mount_path_; }

  bool file_exists(const char *path) override;
  bool get_file_size(const char *path, size_t *size) override;
  bool read_file(const char *path, uint8_t *data, size_t *length) override;
  bool write_file(const char *path, const uint8_t *data, size_t length) override;
  bool append_file(const char *path, const uint8_t *data, size_t length) override;
  bool delete_file(const char *path) override;
  bool rename_file(const char *old_path, const char *new_path) override;
  bool copy_file(const char *src_path, const char *dst_path) override;

  bool dir_exists(const char *path) override;
  bool create_dir(const char *path) override;
  bool delete_dir(const char *path, bool recursive = false) override;
  bool list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) override;

  bool get_space_info(uint64_t *total, uint64_t *free) override;
  bool can_write_file(const char *path, size_t size) override;

  void *open_file(const char *path, const char *mode) override;
  size_t read_file_chunk(void *handle, uint8_t *buffer, size_t size) override;
  size_t write_file_chunk(void *handle, const uint8_t *data, size_t size) override;
  bool seek_file(void *handle, size_t offset) override;
  size_t tell_file(void *handle) override;
  bool close_file(void *handle) override;

  bool format() override;
  bool sync() override;
#endif

 protected:
  uint16_t vid_{0};
  uint16_t pid_{0};
  std::string mount_path_;
  std::string id_;
  bool mounted_{false};
  std::vector<mount_ready_callback_t> mount_ready_callbacks_;

  std::string build_full_path(const char *path);
};

}  // namespace usb_storage
}  // namespace esphome
#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
