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

#include "esp_log.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_helpers.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "esp_timer.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_types_stack.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <dirent.h>
#include <functional>
#include <vector>

namespace esphome {
namespace usb_storage {

static constexpr const char *MNT_PATH = "/usb";
static constexpr uint16_t BUFFER_SIZE = 4096;
static constexpr uint8_t MAX_MSC_DEVICES = CONFIG_FATFS_VOLUME_COUNT;

/**
 * @brief MSC Device Entry
 *
 * This structure holds information about a connected MSC device,
 * including the USB address, MSC device handle, VFS handle, and assigned mount point.
 */
typedef struct {
  uint8_t usb_addr;                    /*!< USB device address */
  msc_host_device_handle_t msc_device; /*!< Handle of the MSC device */
  msc_host_vfs_handle_t vfs_handle;    /*!< VFS handle assigned to the MSC device */
} msc_dev_entry_t;

static constexpr const char *TAG = "usb_storage";
static constexpr uint8_t SCSI_COMMAND_SET = 0x06;
static constexpr uint8_t BULK_ONLY_TRANSFER = 0x50;

class USBStorageHost : public Component {
  friend class USBStorageDevice;

 public:
  void setup() override;

 protected:
  void free_all_msc_devices(void);
  void free_msc_device(int slot);
  int8_t find_free_slot(void);
  esp_err_t allocate_new_msc_device(uint8_t new_dev_address, const std::string &mount_path);
  int8_t find_msc_device_slot(uint8_t usb_addr);
  msc_host_device_handle_t get_handle_by_address(uint8_t usb_addr);

  msc_dev_entry_t *msc_devices_[MAX_MSC_DEVICES] = {NULL};
};

// Forward declaration for mount callback
using mount_ready_callback_t = std::function<void(const std::string &mount_path)>;

#ifdef USE_STORAGE
class USBStorageDevice : public Component,
                         public usb_host::USBClassDriver,
                         public Parented<USBStorageHost>,
                         public storage::StorageDevice {
#else
class USBStorageDevice : public Component, public usb_host::USBClassDriver, public Parented<USBStorageHost> {
#endif
  friend class USBHost;
  friend class USBStorageHost;

 public:
  USBStorageDevice() = default;
  void setup() override;
  void dump_config() override;
  // Run after storage component (DATA=600) to ensure global_storage is initialized
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_mount_path(const std::string &mount_path) { this->mount_path_ = mount_path; }
  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }
  void set_id(const std::string &id) { this->id_ = id; }

  // USBClassDriver interface
  uint8_t interface_class() override { return USB_CLASS_MASS_STORAGE; }
  bool claim_interface(const usb_intf_desc_t *intf_desc, const usb_device_desc_t *dev_desc) override;
  void on_interface_claimed(uint8_t addr, uint8_t interface_num) override;
  void on_device_disconnected(uint8_t addr) override;

  // MSC-specific operations
  void list_files();
  void speed_test();
  void file_operations();
  void print_device_info();
  uint8_t find_usb_addr_by_handle(msc_host_device_handle_t handle);

  // Mount notification system for storage consumers
  void add_mount_ready_callback(const mount_ready_callback_t &callback) {
    this->mount_ready_callbacks_.push_back(callback);
  }
  const std::string &get_mount_path() const { return this->mount_path_; }
  bool is_mounted() const { return this->slot_ >= 0; }

  // Public mount/unmount methods for external control
  bool remount_device();
  void unmount_device();

#ifdef USE_STORAGE
  //========================================================================
  // StorageDevice Interface Implementation
  //========================================================================

  // Device Information (required)
  storage::StorageInfo get_info() override;
  bool is_available() override { return this->slot_ >= 0; }

  // Filesystem Access
  bool supports_filesystem() override { return true; }
  std::string get_mount_path() override { return this->mount_path_; }

  // File Operations
  bool file_exists(const char *path) override;
  bool get_file_size(const char *path, size_t *size) override;
  bool read_file(const char *path, uint8_t *data, size_t *length) override;
  bool write_file(const char *path, const uint8_t *data, size_t length) override;
  bool append_file(const char *path, const uint8_t *data, size_t length) override;
  bool delete_file(const char *path) override;
  bool rename_file(const char *old_path, const char *new_path) override;
  bool copy_file(const char *src_path, const char *dst_path) override;

  // Directory Operations
  bool dir_exists(const char *path) override;
  bool create_dir(const char *path) override;
  bool delete_dir(const char *path, bool recursive = false) override;
  bool list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) override;

  // Space Information
  bool get_space_info(uint64_t *total, uint64_t *free) override;
  bool can_write_file(const char *path, size_t size) override;

  // Streaming File Access
  void *open_file(const char *path, const char *mode) override;
  size_t read_file_chunk(void *handle, uint8_t *buffer, size_t size) override;
  size_t write_file_chunk(void *handle, const uint8_t *data, size_t size) override;
  bool seek_file(void *handle, size_t offset) override;
  size_t tell_file(void *handle) override;
  bool close_file(void *handle) override;

  // Maintenance
  bool format() override;
  bool sync() override;
#endif

 protected:
  uint8_t device_addr_{255};
  std::string mount_path_;
  std::string id_;                                             // Unique identifier for storage registry
  uint16_t vid_{0x0000};                                       // 0x0000 = wildcard, match any VID
  uint16_t pid_{0x0000};                                       // 0x0000 = wildcard, match any PID
  int8_t slot_{-1};                                            // Track which slot this device is using
  std::vector<mount_ready_callback_t> mount_ready_callbacks_;  // Callbacks to notify when mount is ready

  // Helper to build full path
  std::string build_full_path(const char *path);
};

}  // namespace usb_storage
}  // namespace esphome
#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
