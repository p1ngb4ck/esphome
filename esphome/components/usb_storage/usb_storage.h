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

typedef struct {
  uint8_t usb_addr;
  msc_host_device_handle_t msc_device;
  msc_host_vfs_handle_t vfs_handle;
} msc_dev_entry_t;

static constexpr const char *TAG = "usb_storage";
static constexpr uint8_t SCSI_COMMAND_SET = 0x06;
static constexpr uint8_t BULK_ONLY_TRANSFER = 0x50;

// Forward declarations
class USBStorageDevice;
using mount_ready_callback_t = std::function<void(const std::string &mount_path)>;

class USBStorageHost : public Component {
  friend class USBStorageDevice;

 public:
  void setup() override;

  void on_msc_connected(uint8_t addr, uint16_t vid, uint16_t pid);
  void on_msc_removed(uint8_t addr);
  void add_device(USBStorageDevice *device) { this->devices_.push_back(device); }

 protected:
  void free_all_msc_devices(void);
  void free_msc_device(int slot);
  int8_t find_free_slot(void);
  esp_err_t allocate_new_msc_device(uint8_t new_dev_address, const std::string &mount_path);
  int8_t find_msc_device_slot(uint8_t usb_addr);
  msc_host_device_handle_t get_handle_by_address(uint8_t usb_addr);

  msc_dev_entry_t *msc_devices_[MAX_MSC_DEVICES] = {NULL};
  std::vector<USBStorageDevice *> devices_{};
};

// Thin USBClient that detects MSC devices by interface class and delegates to USBStorageHost.
// Overrides setup()/loop() to skip the transfer buffer pool and dedicated USB task — it only
// needs connect/disconnect events, which it polls non-blocking in loop().
class MSCDetector : public usb_host::USBClient {
 public:
  explicit MSCDetector(USBStorageHost *host) : usb_host::USBClient(0, 0), host_(host) {}

  void setup() override;
  void loop() override;

  uint8_t get_interface_class() const override { return USB_CLASS_MASS_STORAGE; }

 protected:
  void on_connected() override;
  void on_removed(usb_device_handle_t handle) override;

  USBStorageHost *host_;
  uint8_t connected_addr_{0};
};

#ifdef USE_STORAGE
class USBStorageDevice : public Component, public Parented<USBStorageHost>, public storage::StorageDevice {
#else
class USBStorageDevice : public Component, public Parented<USBStorageHost> {
#endif
  friend class USBStorageHost;

 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_mount_path(const std::string &mount_path) { this->mount_path_ = mount_path; }
  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }
  void set_id(const std::string &id) { this->id_ = id; }

  // Called by USBStorageHost when an MSC device with matching VID/PID connects/disconnects
  void on_device_connected(uint8_t addr);
  void on_device_disconnected(uint8_t addr);

  void list_files();
  void speed_test();
  void file_operations();
  void print_device_info();
  uint8_t find_usb_addr_by_handle(msc_host_device_handle_t handle);

  void add_mount_ready_callback(const mount_ready_callback_t &callback) {
    this->mount_ready_callbacks_.push_back(callback);
  }
  const std::string &get_mount_path() const { return this->mount_path_; }
  bool is_mounted() const { return this->slot_ >= 0; }

  bool remount_device();
  void unmount_device();

#ifdef USE_STORAGE
  storage::StorageInfo get_info() override;
  bool is_available() override { return this->slot_ >= 0; }

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
  uint8_t device_addr_{255};
  uint16_t vid_{0};
  uint16_t pid_{0};
  std::string mount_path_;
  std::string id_;
  int8_t slot_{-1};
  std::vector<mount_ready_callback_t> mount_ready_callbacks_;

  std::string build_full_path(const char *path);
};

}  // namespace usb_storage
}  // namespace esphome
#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
