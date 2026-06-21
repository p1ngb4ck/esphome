#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32P4)

#include "usb_storage.h"
#include "esphome/core/defines.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

namespace esphome {
namespace usb_storage {

static const usb_standard_desc_t *next_interface_desc(const usb_standard_desc_t *desc, size_t len, size_t *offset) {
  return usb_parse_next_descriptor_of_type(desc, len, USB_W_VALUE_DT_INTERFACE, (int *) offset);
}

static const usb_intf_desc_t *find_msc_interface(const usb_config_desc_t *config_desc) {
  size_t offset = 0;
  size_t total_length = config_desc->wTotalLength;
  const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *) config_desc;

  next_desc = next_interface_desc(next_desc, total_length, &offset);

  while (next_desc) {
    const usb_intf_desc_t *ifc_desc = (const usb_intf_desc_t *) next_desc;

    if (ifc_desc->bInterfaceClass == USB_CLASS_MASS_STORAGE && ifc_desc->bInterfaceSubClass == SCSI_COMMAND_SET &&
        ifc_desc->bInterfaceProtocol == BULK_ONLY_TRANSFER) {
      return ifc_desc;
    }

    next_desc = next_interface_desc(next_desc, total_length, &offset);
  };
  return NULL;
}

int8_t USBStorageHost::find_free_slot(void) {
  for (int i = 0; i < MAX_MSC_DEVICES; i++) {
    if (this->msc_devices_[i] == NULL) {
      ESP_LOGI(TAG, "Found free slot for MSC device at index %d", i);
      return i;
    }
  }
  return -1;
}

int8_t USBStorageHost::find_msc_device_slot(uint8_t usb_addr) {
  for (int i = 0; i < MAX_MSC_DEVICES; i++) {
    if (this->msc_devices_[i] != nullptr && this->msc_devices_[i]->usb_addr == usb_addr) {
      ESP_LOGI(TAG, "Found MSC device slot at index %d", i);
      return i;
    }
  }
  return -1;
}

esp_err_t USBStorageHost::allocate_new_msc_device(uint8_t new_dev_address, const std::string &mount_path) {
  int slot = this->find_free_slot();
  if (slot < 0) {
    ESP_LOGW(TAG, "No free slots for new MSC device (max %d)", MAX_MSC_DEVICES);
    return ESP_ERR_NOT_FOUND;
  }

  ESP_LOGI(TAG, "Allocating slot %d for device address %d with mount path '%s'", slot, new_dev_address,
           mount_path.c_str());

  this->msc_devices_[slot] = (msc_dev_entry_t *) calloc(1, sizeof(msc_dev_entry_t));
  if (this->msc_devices_[slot] == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for new MSC device entry");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Memory allocated, calling msc_host_install_device...");

  esp_err_t err = msc_host_install_device(new_dev_address, &this->msc_devices_[slot]->msc_device);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
    free(this->msc_devices_[slot]);
    this->msc_devices_[slot] = NULL;
    return err;
  }

  ESP_LOGI(TAG, "msc_host_install_device succeeded");

  this->msc_devices_[slot]->usb_addr = new_dev_address;

  ESP_LOGI(TAG, "Stored USB address, proceeding with VFS registration...");

  const esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 1024,
  };

  err = msc_host_vfs_register(this->msc_devices_[slot]->msc_device, mount_path.c_str(), &mount_config,
                              &this->msc_devices_[slot]->vfs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
    esp_err_t res = (msc_host_uninstall_device(this->msc_devices_[slot]->msc_device));
    if (res != ESP_OK) {
      ESP_LOGE(TAG, "msc_host_uninstall_device failed during cleanup: %s", esp_err_to_name(res));
    }
    free(this->msc_devices_[slot]);
    this->msc_devices_[slot] = NULL;
    return err;
  }
  return ESP_OK;
}

void USBStorageHost::free_msc_device(int slot) {
  if (slot < 0 || slot >= MAX_MSC_DEVICES || !this->msc_devices_[slot]) {
    ESP_LOGE(TAG, "Invalid slot index for MSC device deallocation");
    return;
  }

  if (this->msc_devices_[slot]->vfs_handle) {
    ESP_ERROR_CHECK(msc_host_vfs_unregister(this->msc_devices_[slot]->vfs_handle));
  }
  if (this->msc_devices_[slot]->msc_device) {
    ESP_ERROR_CHECK(msc_host_uninstall_device(this->msc_devices_[slot]->msc_device));
  }

  free(this->msc_devices_[slot]);
  this->msc_devices_[slot] = NULL;
}

void USBStorageHost::free_all_msc_devices(void) {
  for (int i = 0; i < MAX_MSC_DEVICES; i++) {
    if (this->msc_devices_[i]) {
      free_msc_device(i);
    }
  }
}

uint8_t USBStorageDevice::find_usb_addr_by_handle(msc_host_device_handle_t handle) {
  for (uint8_t i = 0; i < MAX_MSC_DEVICES; i++) {
    if (this->parent_->msc_devices_[i] && this->parent_->msc_devices_[i]->msc_device == handle) {
      return this->parent_->msc_devices_[i]->usb_addr;
    }
  }
  return -1;
}

msc_host_device_handle_t USBStorageHost::get_handle_by_address(uint8_t usb_addr) {
  for (uint8_t i = 0; i < MAX_MSC_DEVICES; i++) {
    if (this->msc_devices_[i] && this->msc_devices_[i]->usb_addr == usb_addr) {
      return this->msc_devices_[i]->msc_device;
    }
  }
  return nullptr;
}

void USBStorageDevice::print_device_info() {
  int8_t slot = this->parent_->find_msc_device_slot(this->device_addr_);
  if (slot < 0) {
    ESP_LOGE(TAG, "Device slot not found for printing device info");
    return;
  }
  msc_host_device_handle_t handle = this->parent_->msc_devices_[slot]->msc_device;

  msc_host_device_info_t info;
  esp_err_t err = msc_host_get_device_info(handle, &info);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "msc_host_get_device_info failed: %s", esp_err_to_name(err));
    return;
  }
  const size_t megabyte = 1024 * 1024;
  uint64_t capacity = ((uint64_t) info.sector_size * info.sector_count) / megabyte;

  ESP_LOGI(TAG, "Device info:\n");
  ESP_LOGI(TAG, "\t Capacity: %llu MB\n", capacity);
  ESP_LOGI(TAG, "\t Sector size: %" PRIu32 "\n", info.sector_size);
  ESP_LOGI(TAG, "\t Sector count: %" PRIu32 "\n", info.sector_count);
  ESP_LOGI(TAG, "\t PID: 0x%04X \n", info.idProduct);
  ESP_LOGI(TAG, "\t VID: 0x%04X \n", info.idVendor);
#ifndef CONFIG_LIBC_NEWLIB_NANO_FORMAT
  ESP_LOGI(TAG, "\t iProduct: %S \n", info.iProduct);
  ESP_LOGI(TAG, "\t iManufacturer: %S \n", info.iManufacturer);
  ESP_LOGI(TAG, "\t iSerialNumber: %S \n", info.iSerialNumber);
#endif
}

void USBStorageDevice::file_operations() {
  std::string directory = this->mount_path_ + "/esp";
  std::string file_path = this->mount_path_ + "/esp/test.txt";

  struct stat s = {0};
  bool directory_exists = stat(directory.c_str(), &s) == 0;
  if (!directory_exists) {
    if (mkdir(directory.c_str(), 0775) != 0) {
      ESP_LOGE(TAG, "mkdir failed with errno: %s", strerror(errno));
    }
  }

  if (stat(file_path.c_str(), &s) != 0) {
    ESP_LOGI(TAG, "Creating file");
    FILE *f = fopen(file_path.c_str(), "w");
    if (f == NULL) {
      ESP_LOGE(TAG, "Failed to open file for writing");
      return;
    }
    fprintf(f, "Hello World!\n");
    fclose(f);
  }

  FILE *f;
  ESP_LOGI(TAG, "Reading file");
  f = fopen(file_path.c_str(), "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for reading");
    return;
  }
  char line[64];
  fgets(line, sizeof(line), f);
  fclose(f);
  char *pos = strchr(line, '\n');
  if (pos) {
    *pos = '\0';
  }
  ESP_LOGI(TAG, "Read from file '%s': '%s'", file_path.c_str(), line);
}

void USBStorageDevice::speed_test() {
#define ITERATIONS 256
  int64_t test_start, test_end;
  std::string test_file = this->mount_path_ + "/esp/dummy";

  FILE *f = fopen(test_file.c_str(), "wb+");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing");
    return;
  }
  setvbuf(f, NULL, _IOFBF, BUFFER_SIZE);

  void *data = malloc(BUFFER_SIZE);
  assert(data);

  ESP_LOGI(TAG, "Writing to file %s", test_file.c_str());
  test_start = esp_timer_get_time();
  for (int i = 0; i < ITERATIONS; i++) {
    if (fwrite(data, BUFFER_SIZE, 1, f) == 0) {
      ESP_LOGE(TAG, "Write error");
      fclose(f);
      free(data);
      return;
    }
  }
  test_end = esp_timer_get_time();
  ESP_LOGI(TAG, "Write speed %1.2f MiB/s", (BUFFER_SIZE * ITERATIONS) / (float) (test_end - test_start));
  rewind(f);

  ESP_LOGI(TAG, "Reading from file %s", test_file.c_str());
  test_start = esp_timer_get_time();
  for (int i = 0; i < ITERATIONS; i++) {
    if (0 == fread(data, BUFFER_SIZE, 1, f)) {
      ESP_LOGE(TAG, "Read error");
      fclose(f);
      free(data);
      return;
    }
  }
  test_end = esp_timer_get_time();
  ESP_LOGI(TAG, "Read speed %1.2f MiB/s", (BUFFER_SIZE * ITERATIONS) / (float) (test_end - test_start));

  fclose(f);
  free(data);
}

void USBStorageDevice::list_files() {
  ESP_LOGI(TAG, "Listing contents of '%s'", this->mount_path_.c_str());

  struct dirent *d;
  DIR *dh = opendir(this->mount_path_.c_str());
  if (!dh) {
    ESP_LOGE(TAG, "Failed to open directory: %s", this->mount_path_.c_str());
    return;
  }

  while ((d = readdir(dh)) != NULL) {
    ESP_LOGI(TAG, "  %s/%s", this->mount_path_.c_str(), d->d_name);
  }
  closedir(dh);
}

static void msc_event_callback(const msc_host_event_t *event, void *arg) {
  // Intentionally empty - we handle device events through usb_host component
}

void USBStorageHost::setup() {
  ESP_LOGCONFIG(TAG, "Registering USB Storage Host Component...");
  const msc_host_driver_config_t msc_config = {
      .create_backround_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .callback = msc_event_callback,
  };

  esp_err_t err = msc_host_install(&msc_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize MSC host driver: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "MSC host driver initialized successfully");
}

void USBStorageHost::on_msc_connected(uint8_t addr, uint16_t vid, uint16_t pid) {
  for (auto *device : this->devices_) {
    // wildcard (0,0) matches any; otherwise match VID and PID
    if ((device->vid_ == 0 && device->pid_ == 0) ||
        (device->vid_ == vid && device->pid_ == pid)) {
      if (device->device_addr_ == 255) {  // not already claimed
        device->on_device_connected(addr);
        return;
      }
    }
  }
  ESP_LOGW(TAG, "No storage device configured for MSC addr=%d VID=0x%04X PID=0x%04X", addr, vid, pid);
}

void USBStorageHost::on_msc_removed(uint8_t addr) {
  for (auto *device : this->devices_) {
    if (device->device_addr_ == addr) {
      device->on_device_disconnected(addr);
      return;
    }
  }
}

void MSCDetector::setup() {
  // Only register as a USB host client — skip transfer buffer pool and dedicated task.
  // MSCDetector never does transfers; it only needs connect/disconnect events.
  usb_host_client_config_t config{.is_synchronous = false,
                                  .max_num_event_msg = 5,
                                  .async = {.client_event_callback = usb_host::USBClient::client_event_cb, .callback_arg = this}};
  auto err = usb_host_client_register(&config, &this->handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "MSCDetector client register failed: %s", esp_err_to_name(err));
    this->mark_failed();
  }
}

void MSCDetector::loop() {
  // Poll events non-blocking instead of using a dedicated task
  usb_host_client_handle_events(this->handle_, 0);
  this->process_usb_events_();
}

void MSCDetector::on_connected() {
  const usb_device_desc_t *desc;
  uint16_t vid = 0, pid = 0;
  if (usb_host_get_device_descriptor(this->device_handle_, &desc) == ESP_OK) {
    vid = desc->idVendor;
    pid = desc->idProduct;
  }
  this->connected_addr_ = static_cast<uint8_t>(this->device_addr_);
  this->host_->on_msc_connected(this->connected_addr_, vid, pid);
  // Release our handle — MSC host driver will open the device independently
  this->disconnect();
}

void MSCDetector::on_removed(usb_device_handle_t handle) {
  if (this->connected_addr_ != 0) {
    this->host_->on_msc_removed(this->connected_addr_);
    this->connected_addr_ = 0;
  }
  USBClient::on_removed(handle);
}

void USBStorageDevice::setup() { ESP_LOGCONFIG(TAG, "Setting up USB Storage Device"); }

void USBStorageDevice::dump_config() {
  ESP_LOGCONFIG(TAG, "USB Storage Device:");
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->mount_path_.c_str());
}

void USBStorageDevice::on_device_connected(uint8_t addr) {
  ESP_LOGI(TAG, "USB Storage Device connected (address=%d, mount_path='%s')", addr, this->mount_path_.c_str());

  this->device_addr_ = addr;

  esp_err_t err = this->parent_->allocate_new_msc_device(addr, this->mount_path_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to allocate new MSC device: %s", esp_err_to_name(err));
    return;
  }

  this->slot_ = this->parent_->find_msc_device_slot(addr);
  if (this->slot_ < 0) {
    ESP_LOGE(TAG, "Failed to find slot for newly allocated device!");
    return;
  }

  ESP_LOGI(TAG, "Successfully allocated MSC device to slot %d", this->slot_);

  this->print_device_info();

  ESP_LOGI(TAG, "Notifying %zu mount ready callbacks for '%s'", this->mount_ready_callbacks_.size(),
           this->mount_path_.c_str());
  for (const auto &callback : this->mount_ready_callbacks_) {
    callback(this->mount_path_);
  }

#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_device(this);
    ESP_LOGD(TAG, "Registered USB storage device with storage registry");
  }
#endif
}

void USBStorageDevice::on_device_disconnected(uint8_t addr) {
  if (this->device_addr_ != addr) {
    return;
  }

  ESP_LOGI(TAG, "USB Storage Device disconnected (address=%d)", addr);

  int8_t slot = this->parent_->find_msc_device_slot(addr);
  if (slot < 0) {
    ESP_LOGE(TAG, "Could not find MSC device slot for disconnected device");
  } else {
    this->parent_->free_msc_device(slot);
    ESP_LOGI(TAG, "Freed MSC device resources for slot %d", slot);
  }

  this->device_addr_ = 255;
  this->slot_ = -1;

#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->unregister_device(this);
    ESP_LOGD(TAG, "Unregistered USB storage device from storage registry");
  }
#endif
}

bool USBStorageDevice::remount_device() {
  if (this->device_addr_ == 255) {
    ESP_LOGW(TAG, "No device connected, cannot remount");
    return false;
  }

  uint8_t addr = this->device_addr_;

  if (this->slot_ >= 0) {
    this->unmount_device();
  }

  esp_err_t err = this->parent_->allocate_new_msc_device(addr, this->mount_path_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to remount MSC device: %s", esp_err_to_name(err));
    return false;
  }

  this->slot_ = this->parent_->find_msc_device_slot(this->device_addr_);
  if (this->slot_ < 0) {
    ESP_LOGE(TAG, "Failed to find slot for remounted device!");
    return false;
  }

  ESP_LOGI(TAG, "Device remounted successfully to '%s'", this->mount_path_.c_str());

  for (const auto &callback : this->mount_ready_callbacks_) {
    callback(this->mount_path_);
  }

#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->notify_device_changed(this);
  }
#endif

  return true;
}

void USBStorageDevice::unmount_device() {
  if (this->slot_ < 0) {
    ESP_LOGD(TAG, "Device not mounted, nothing to unmount");
    return;
  }

  ESP_LOGI(TAG, "Unmounting MSC device from slot %d (mount path: '%s')", this->slot_, this->mount_path_.c_str());
  this->parent_->free_msc_device(this->slot_);
  this->slot_ = -1;
  ESP_LOGI(TAG, "Device unmounted successfully");

#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->notify_device_changed(this);
  }
#endif
}

// Helper to build full path
std::string USBStorageDevice::build_full_path(const char *path) {
  std::string full_path = this->mount_path_;
  if (path[0] != '/') {
    full_path += "/";
  }
  full_path += path;
  return full_path;
}

#ifdef USE_STORAGE
//========================================================================
// StorageDevice Interface Implementation
//========================================================================

storage::StorageInfo USBStorageDevice::get_info() {
  storage::StorageInfo info;
  info.id = this->id_.empty() ? "usb_storage" : this->id_;
  info.name = "USB Storage";
  info.type = storage::StorageType::USB_MSC;
  info.filesystem = storage::FilesystemType::FAT;
  info.mount_path = this->mount_path_;
  info.is_mounted = this->slot_ >= 0;
  info.is_removable = true;
  info.is_read_only = false;
  info.supports_raw_access = false;
  info.supports_filesystem = true;

  // Get space info
  if (this->slot_ >= 0) {
    uint64_t total, free_bytes;
    if (this->get_space_info(&total, &free_bytes)) {
      info.total_bytes = total;
      info.free_bytes = free_bytes;
      info.block_size = 512;
    } else {
      info.total_bytes = 0;
      info.free_bytes = 0;
      info.block_size = 512;
    }
  } else {
    info.total_bytes = 0;
    info.free_bytes = 0;
    info.block_size = 512;
  }

  return info;
}

bool USBStorageDevice::file_exists(const char *path) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;
  return stat(full_path.c_str(), &path_stat) == 0 && S_ISREG(path_stat.st_mode);
}

bool USBStorageDevice::get_file_size(const char *path, size_t *size) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;

  if (stat(full_path.c_str(), &path_stat) != 0) {
    return false;
  }

  *size = path_stat.st_size;
  return true;
}

bool USBStorageDevice::read_file(const char *path, uint8_t *data, size_t *length) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  FILE *f = fopen(full_path.c_str(), "rb");
  if (f == nullptr)
    return false;

  size_t read = fread(data, 1, *length, f);
  fclose(f);
  *length = read;
  return true;
}

bool USBStorageDevice::write_file(const char *path, const uint8_t *data, size_t length) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  FILE *f = fopen(full_path.c_str(), "wb");
  if (f == nullptr)
    return false;

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool USBStorageDevice::append_file(const char *path, const uint8_t *data, size_t length) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  FILE *f = fopen(full_path.c_str(), "ab");
  if (f == nullptr)
    return false;

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool USBStorageDevice::delete_file(const char *path) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  return remove(full_path.c_str()) == 0;
}

bool USBStorageDevice::rename_file(const char *old_path, const char *new_path) {
  if (this->slot_ < 0)
    return false;

  std::string full_old = this->build_full_path(old_path);
  std::string full_new = this->build_full_path(new_path);
  return rename(full_old.c_str(), full_new.c_str()) == 0;
}

bool USBStorageDevice::copy_file(const char *src_path, const char *dst_path) {
  if (this->slot_ < 0)
    return false;

  std::string full_src = this->build_full_path(src_path);
  std::string full_dst = this->build_full_path(dst_path);

  FILE *src = fopen(full_src.c_str(), "rb");
  if (src == nullptr)
    return false;

  FILE *dst = fopen(full_dst.c_str(), "wb");
  if (dst == nullptr) {
    fclose(src);
    return false;
  }

  uint8_t buffer[512];
  size_t bytes_read;
  bool success = true;

  while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
    if (fwrite(buffer, 1, bytes_read, dst) != bytes_read) {
      success = false;
      break;
    }
  }

  fclose(src);
  fclose(dst);
  return success;
}

bool USBStorageDevice::dir_exists(const char *path) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;
  return stat(full_path.c_str(), &path_stat) == 0 && S_ISDIR(path_stat.st_mode);
}

bool USBStorageDevice::create_dir(const char *path) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  return mkdir(full_path.c_str(), 0755) == 0;
}

bool USBStorageDevice::delete_dir(const char *path, bool recursive) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);

  if (recursive) {
    DIR *dir = opendir(full_path.c_str());
    if (dir == nullptr)
      return false;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        continue;

      std::string entry_path = std::string(path) + "/" + entry->d_name;
      if (entry->d_type == DT_DIR) {
        if (!this->delete_dir(entry_path.c_str(), true)) {
          closedir(dir);
          return false;
        }
      } else {
        if (!this->delete_file(entry_path.c_str())) {
          closedir(dir);
          return false;
        }
      }
    }
    closedir(dir);
  }

  return rmdir(full_path.c_str()) == 0;
}

bool USBStorageDevice::list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) {
  if (this->slot_ < 0)
    return false;

  std::string full_path = this->build_full_path(path);
  DIR *dir = opendir(full_path.c_str());
  if (dir == nullptr)
    return false;

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    storage::StorageFileInfo info;
    info.name = entry->d_name;
    info.path = std::string(path) + "/" + entry->d_name;
    info.is_directory = entry->d_type == DT_DIR;

    std::string entry_full_path = full_path + "/" + entry->d_name;
    struct stat st;
    if (stat(entry_full_path.c_str(), &st) == 0) {
      info.size = info.is_directory ? 0 : st.st_size;
      info.modified_time = st.st_mtime;
    } else {
      info.size = 0;
      info.modified_time = 0;
    }

    entries->push_back(info);
  }

  closedir(dir);
  return true;
}

bool USBStorageDevice::get_space_info(uint64_t *total, uint64_t *free) {
  if (this->slot_ < 0)
    return false;

  FATFS *fs;
  DWORD fre_clust;

  // Get volume information and free clusters
  // Need to append "/" to mount path for f_getfree
  std::string path = this->mount_path_;
  if (path.back() != '/') {
    path += '/';
  }

  FRESULT res = f_getfree(path.c_str(), &fre_clust, &fs);
  if (res != FR_OK) {
    ESP_LOGW(TAG, "Failed to get filesystem info: %d", res);
    return false;
  }

  // Calculate total and free bytes
  // Cast to uint64_t before multiplication to avoid overflow on large drives
  DWORD tot_sect = (fs->n_fatent - 2) * fs->csize;
  DWORD fre_sect = fre_clust * fs->csize;

  // Sector size from filesystem
  *total = (uint64_t) tot_sect * fs->ssize;
  *free = (uint64_t) fre_sect * fs->ssize;
  return true;
}

bool USBStorageDevice::can_write_file(const char *path, size_t size) {
  if (this->slot_ < 0)
    return false;

  uint64_t total, free_space;
  if (!this->get_space_info(&total, &free_space))
    return false;

  return free_space >= size;
}

void *USBStorageDevice::open_file(const char *path, const char *mode) {
  if (this->slot_ < 0)
    return nullptr;

  std::string full_path = this->build_full_path(path);
  return fopen(full_path.c_str(), mode);
}

size_t USBStorageDevice::read_file_chunk(void *handle, uint8_t *buffer, size_t size) {
  if (handle == nullptr)
    return 0;
  return fread(buffer, 1, size, static_cast<FILE *>(handle));
}

size_t USBStorageDevice::write_file_chunk(void *handle, const uint8_t *data, size_t size) {
  if (handle == nullptr)
    return 0;
  return fwrite(data, 1, size, static_cast<FILE *>(handle));
}

bool USBStorageDevice::seek_file(void *handle, size_t offset) {
  if (handle == nullptr)
    return false;
  return fseek(static_cast<FILE *>(handle), offset, SEEK_SET) == 0;
}

size_t USBStorageDevice::tell_file(void *handle) {
  if (handle == nullptr)
    return 0;
  return ftell(static_cast<FILE *>(handle));
}

bool USBStorageDevice::close_file(void *handle) {
  if (handle == nullptr)
    return false;
  return fclose(static_cast<FILE *>(handle)) == 0;
}

bool USBStorageDevice::format() {
  ESP_LOGW(TAG, "Format not implemented for USB storage");
  return false;
}

bool USBStorageDevice::sync() { return true; }

#endif  // USE_STORAGE

}  // namespace usb_storage
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
