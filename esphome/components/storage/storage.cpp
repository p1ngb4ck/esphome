#include "storage.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"  // For App.feed_wdt()
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <algorithm>

// Include yield function for ESP32/ESP8266
#ifdef ESP32
#include <esp_vfs_fat.h>  // For esp_vfs_fat_info()
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#define yield() taskYIELD()
#elif defined(ESP8266)
#include <Esp.h>
// yield() is already available on ESP8266
#else
// Fallback for other platforms
#define yield() delayMicroseconds(1)
#endif

// PSRAM support (only on ESP-IDF with guaranteed PSRAM)
#if defined(USE_ESP_IDF) && defined(USE_PSRAM)
#include <esp_heap_caps.h>
#define STORAGE_USE_PSRAM_POOL 1
#else
#define STORAGE_USE_PSRAM_POOL 0
#endif

namespace esphome {
namespace storage {

static const char *const TAG = "storage";

// Global accessor for soft dependency pattern
Storage *global_storage = nullptr;

// =====================================================
// StorageMount Implementation
// =====================================================

bool StorageMount::is_available() const {
  if (this->storage_ == nullptr) {
    return false;
  }

  // Check if the mount path exists
  struct stat st;
  if (stat(this->path_.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    return true;
  }

  return false;
}

bool StorageMount::get_stats(uint64_t &total_bytes, uint64_t &free_bytes) const {
#ifdef ESP32
  // Use ESP-IDF VFS FAT API to get filesystem stats
  esp_err_t ret = esp_vfs_fat_info(this->path_.c_str(), &total_bytes, &free_bytes);
  return (ret == ESP_OK);
#else
  // Not implemented for other platforms
  return false;
#endif
}

// =====================================================
// Storage Implementation
// =====================================================

void Storage::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Storage Host Component...");

  // Set global accessor for soft dependency pattern
  global_storage = this;

  // Initialize PSRAM buffer pool (only on devices with PSRAM)
#if STORAGE_USE_PSRAM_POOL
  this->psram_available_ = true;
  size_t psram_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (psram_size < 256 * 1024) {
    ESP_LOGW(TAG, "Insufficient PSRAM (%zu bytes), buffer pool disabled", psram_size);
    this->psram_available_ = false;
  } else {
    // Allocate 4× 64KB buffers from PSRAM
    for (size_t i = 0; i < MAX_BUFFER_SLOTS; i++) {
      this->buffer_pool_[i].ptr = static_cast<uint8_t *>(heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (this->buffer_pool_[i].ptr != nullptr) {
        this->buffer_pool_[i].size = 65536;
        this->buffer_pool_[i].in_use = false;
        ESP_LOGD(TAG, "Allocated PSRAM buffer slot %zu: 64KB", i);
      } else {
        ESP_LOGW(TAG, "Failed to allocate PSRAM buffer slot %zu", i);
      }
    }
    ESP_LOGCONFIG(TAG, "PSRAM Buffer Pool: 4× 64KB = 256KB total");
  }
#else
  this->psram_available_ = false;
  ESP_LOGD(TAG, "PSRAM buffer pool not available on this platform");
#endif

  ESP_LOGCONFIG(TAG, "  Mounts configured: %zu", this->mounts_.size());
  for (const auto &mount : this->mounts_) {
    ESP_LOGCONFIG(TAG, "    - %s (platform: %s)", mount.path.c_str(), mount.platform.c_str());
  }

  ESP_LOGCONFIG(TAG, "  Device nodes configured: %zu", this->device_nodes_.size());
  for (const auto &node : this->device_nodes_) {
    ESP_LOGCONFIG(TAG, "    - %s (type: %s)", node.path.c_str(), node.device_type.c_str());
  }
}

void Storage::loop() {
  // Nothing to do in loop
}

void Storage::dump_config() {
  ESP_LOGCONFIG(TAG, "Storage Host Component:");
  ESP_LOGCONFIG(TAG, "  Mounts: %zu", this->mounts_.size());
  for (const auto &mount : this->mounts_) {
    ESP_LOGCONFIG(TAG, "    - %s (platform: %s)", mount.path.c_str(), mount.platform.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Device Nodes (/dev): %zu", this->device_nodes_.size());
  for (const auto &node : this->device_nodes_) {
    ESP_LOGCONFIG(TAG, "    - %s (type: %s)", node.path.c_str(), node.device_type.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Network Storage: %zu", this->network_storage_.size());
  for (const auto *storage : this->network_storage_) {
    ESP_LOGCONFIG(TAG, "    - %s (protocol: %s, connected: %s)", storage->get_mount_path().c_str(),
                  storage->get_protocol(), storage->is_connected() ? "yes" : "no");
  }
}

void Storage::register_mount(const std::string &path, const std::string &platform, MountSpaceProvider *space_provider) {
  this->mounts_.push_back({path, platform, space_provider});
  ESP_LOGD(TAG, "Registered mount: %s (platform: %s)", path.c_str(), platform.c_str());
}

std::string Storage::find_mount_for_path(const std::string &path) {
  // Find the longest matching mount point
  std::string best_mount;
  size_t best_length = 0;

  for (const auto &mount : this->mounts_) {
    if (path.compare(0, mount.path.length(), mount.path) == 0) {
      if (mount.path.length() > best_length) {
        best_mount = mount.path;
        best_length = mount.path.length();
      }
    }
  }

  return best_mount;
}

// =====================================================
// Device Node Management
// =====================================================

void Storage::register_device_node(const std::string &path, binary_storage::BinaryStorage *device,
                                   const std::string &device_type) {
  this->device_nodes_.push_back({path, device, device_type});
  ESP_LOGD(TAG, "Registered device node: %s (type: %s, device: %p)", path.c_str(), device_type.c_str(), device);
}

bool Storage::is_device_node(const std::string &path) const {
  for (const auto &node : this->device_nodes_) {
    if (node.path == path) {
      return true;
    }
  }
  return false;
}

DeviceNode *Storage::find_device_node(const std::string &path) {
  for (auto &node : this->device_nodes_) {
    if (node.path == path) {
      return &node;
    }
  }
  return nullptr;
}

// =====================================================
// Network Storage Management
// =====================================================

void Storage::register_network_storage(NetworkStorage *storage) {
  if (storage == nullptr) {
    ESP_LOGW(TAG, "Attempted to register null network storage");
    return;
  }

  // Check if already registered (prevent duplicates)
  for (const auto *existing : this->network_storage_) {
    if (existing == storage) {
      ESP_LOGV(TAG, "Network storage already registered: %s", storage->get_mount_path().c_str());
      return;
    }
  }

  this->network_storage_.push_back(storage);
  ESP_LOGD(TAG, "Registered network storage: %s (protocol: %s, mount: %s)", storage->get_protocol(),
           storage->get_protocol(), storage->get_mount_path().c_str());
}

NetworkStorage *Storage::find_network_storage_for_path(const std::string &path) {
  // Find the network storage with the longest matching mount path
  NetworkStorage *best_match = nullptr;
  size_t best_length = 0;

  for (auto *storage : this->network_storage_) {
    const std::string &mount_path = storage->get_mount_path();
    if (path.compare(0, mount_path.length(), mount_path) == 0) {
      if (mount_path.length() > best_length) {
        best_match = storage;
        best_length = mount_path.length();
      }
    }
  }

  return best_match;
}

bool Storage::is_network_path(const std::string &path) const {
  for (const auto *storage : this->network_storage_) {
    const std::string &mount_path = storage->get_mount_path();
    if (path.compare(0, mount_path.length(), mount_path) == 0) {
      return true;
    }
  }
  return false;
}

// =====================================================
// Network Storage File Operations
// =====================================================

bool Storage::network_file_exists(const std::string &path) {
  NetworkStorage *storage = this->find_network_storage_for_path(path);
  if (storage != nullptr && storage->is_connected()) {
    return storage->file_exists(path);
  }
  return false;
}

bool Storage::network_read_file(const std::string &path, std::vector<uint8_t> &data) {
  NetworkStorage *storage = this->find_network_storage_for_path(path);
  if (storage != nullptr && storage->is_connected()) {
    return storage->read_file(path, data);
  }
  ESP_LOGW(TAG, "No connected network storage found for path: %s", path.c_str());
  return false;
}

bool Storage::network_write_file(const std::string &path, const uint8_t *data, size_t length) {
  NetworkStorage *storage = this->find_network_storage_for_path(path);
  if (storage != nullptr && storage->is_connected()) {
    return storage->write_file(path, data, length);
  }
  ESP_LOGW(TAG, "No connected network storage found for path: %s", path.c_str());
  return false;
}

bool Storage::network_delete_file(const std::string &path) {
  NetworkStorage *storage = this->find_network_storage_for_path(path);
  if (storage != nullptr && storage->is_connected()) {
    return storage->delete_file(path);
  }
  ESP_LOGW(TAG, "No connected network storage found for path: %s", path.c_str());
  return false;
}

bool Storage::network_list_directory(const std::string &path, std::vector<NetworkStorage::DirEntry> &entries) {
  NetworkStorage *storage = this->find_network_storage_for_path(path);
  if (storage != nullptr && storage->is_connected()) {
    return storage->list_directory(path, entries);
  }
  ESP_LOGW(TAG, "No connected network storage found for path: %s", path.c_str());
  return false;
}

bool Storage::network_create_directory(const std::string &path) {
  NetworkStorage *storage = this->find_network_storage_for_path(path);
  if (storage != nullptr && storage->is_connected()) {
    return storage->create_directory(path);
  }
  ESP_LOGW(TAG, "No connected network storage found for path: %s", path.c_str());
  return false;
}

bool Storage::network_delete_directory(const std::string &path) {
  NetworkStorage *storage = this->find_network_storage_for_path(path);
  if (storage != nullptr && storage->is_connected()) {
    return storage->delete_directory(path);
  }
  ESP_LOGW(TAG, "No connected network storage found for path: %s", path.c_str());
  return false;
}

bool Storage::file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string Storage::read_file(const std::string &path) {
  FILE *file = fopen(path.c_str(), "rb");

  if (!file) {
    ESP_LOGE(TAG, "Failed to open file: %s (errno: %d)", path.c_str(), errno);
    return "";
  }

  // Get file size safely
  if (fseek(file, 0, SEEK_END) != 0) {
    ESP_LOGE(TAG, "Failed to seek to end of file: %s", path.c_str());
    fclose(file);
    return "";
  }

  long size = ftell(file);
  if (size < 0 || size > 10 * 1024 * 1024) {  // 10MB limit
    ESP_LOGE(TAG, "Invalid file size: %ld bytes", size);
    fclose(file);
    return "";
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    ESP_LOGE(TAG, "Failed to seek to beginning of file: %s", path.c_str());
    fclose(file);
    return "";
  }

  std::string data(size, '\0');
  size_t read_size = fread(&data[0], 1, size, file);
  fclose(file);

  if (read_size != static_cast<size_t>(size)) {
    ESP_LOGE(TAG, "Failed to read complete file: expected %ld, got %zu", size, read_size);
    return "";
  }

  return data;
}

bool Storage::write_file(const std::string &path, const std::string &data) {
  FILE *file = fopen(path.c_str(), "wb");

  if (!file) {
    ESP_LOGE(TAG, "Failed to create file: %s", path.c_str());
    return false;
  }

  size_t written = fwrite(data.data(), 1, data.size(), file);
  fclose(file);

  return written == data.size();
}

std::vector<std::string> Storage::list_files(const std::string &path) {
  std::vector<std::string> files;

  DIR *dir = opendir(path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Cannot open directory: %s", path.c_str());
    return files;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_type == DT_REG) {
      files.push_back(entry->d_name);
    }
  }

  closedir(dir);
  return files;
}

// =====================================================
// StorageDevice Registry Implementation
// =====================================================

void Storage::register_device(StorageDevice *device) {
  if (device == nullptr) {
    ESP_LOGW(TAG, "Attempted to register null storage device");
    return;
  }

  // Check if already registered
  for (auto *existing : this->devices_) {
    if (existing == device) {
      ESP_LOGW(TAG, "Storage device already registered");
      return;
    }
  }

  this->devices_.push_back(device);
  StorageInfo info = device->get_info();
  ESP_LOGD(TAG, "Registered storage device: %s (id: %s, type: %d)", info.name.c_str(), info.id.c_str(),
           static_cast<int>(info.type));

  // Also register mount path for FileManager compatibility
  // This bridges the old mount system with the new device registry
  if (!info.mount_path.empty()) {
    // Determine platform string based on storage type
    std::string platform;
    switch (info.type) {
      case StorageType::SD_CARD:
        platform = "sd_mmc";
        break;
      case StorageType::USB_MSC:
        platform = "usb_msc";
        break;
      case StorageType::BINARY_STORAGE:
        platform = "binary_storage";
        break;
      default:
        platform = "unknown";
        break;
    }
    this->register_mount(info.mount_path, platform);
  }

  // Call callbacks
  this->on_device_added_callbacks_.call(device);
}

void Storage::unregister_device(StorageDevice *device) {
  if (device == nullptr) {
    return;
  }

  auto it = std::find(this->devices_.begin(), this->devices_.end(), device);
  if (it != this->devices_.end()) {
    StorageInfo info = device->get_info();
    ESP_LOGD(TAG, "Unregistering storage device: %s", info.id.c_str());

    this->devices_.erase(it);

    // Also unregister mount path for FileManager compatibility
    if (!info.mount_path.empty()) {
      // Remove from mounts_ StaticVector (manually since StaticVector doesn't have erase())
      StaticVector<MountEntry, MAX_MOUNT_POINTS> new_mounts;
      for (size_t i = 0; i < this->mounts_.size(); i++) {
        if (this->mounts_[i].path != info.mount_path) {
          new_mounts.push_back(this->mounts_[i]);
        } else {
          ESP_LOGD(TAG, "Removing mount point: %s", info.mount_path.c_str());
        }
      }
      this->mounts_ = new_mounts;
    }

    // Call callbacks
    this->on_device_removed_callbacks_.call(device);
  }
}

void Storage::notify_device_changed(StorageDevice *device) {
  if (device == nullptr) {
    return;
  }

  // Verify device is registered
  auto it = std::find(this->devices_.begin(), this->devices_.end(), device);
  if (it != this->devices_.end()) {
    this->on_device_changed_callbacks_.call(device);
  }
}

std::vector<StorageInfo> Storage::get_available_storages() {
  std::vector<StorageInfo> result;

  for (auto *device : this->devices_) {
    if (device->is_available()) {
      result.push_back(device->get_info());
    }
  }

  return result;
}

StorageDevice *Storage::get_device_by_id(const std::string &id) {
  for (auto *device : this->devices_) {
    StorageInfo info = device->get_info();
    if (info.id == id) {
      return device;
    }
  }
  return nullptr;
}

StorageDevice *Storage::get_device_by_mount_path(const std::string &mount_path) {
  for (auto *device : this->devices_) {
    if (device->supports_filesystem()) {
      std::string device_mount = device->get_mount_path();
      if (device_mount == mount_path) {
        return device;
      }
    }
  }
  return nullptr;
}

std::vector<StorageDevice *> Storage::get_devices_by_type(StorageType type) {
  std::vector<StorageDevice *> result;

  for (auto *device : this->devices_) {
    StorageInfo info = device->get_info();
    if (info.type == type) {
      result.push_back(device);
    }
  }

  return result;
}

//========================================================================
// Shared PSRAM Buffer Pool
//========================================================================

uint8_t *Storage::allocate_buffer(size_t size) {
  if (!this->psram_available_ || size > 65536) {
    return nullptr;
  }

  portENTER_CRITICAL(&this->buffer_pool_mutex_);

  // Find first available buffer that fits
  for (size_t i = 0; i < MAX_BUFFER_SLOTS; i++) {
    if (this->buffer_pool_[i].ptr != nullptr && !this->buffer_pool_[i].in_use && this->buffer_pool_[i].size >= size) {
      this->buffer_pool_[i].in_use = true;
      portEXIT_CRITICAL(&this->buffer_pool_mutex_);
      return this->buffer_pool_[i].ptr;
    }
  }

  portEXIT_CRITICAL(&this->buffer_pool_mutex_);
  return nullptr;
}

void Storage::free_buffer(uint8_t *buffer) {
  if (buffer == nullptr) {
    return;
  }

  portENTER_CRITICAL(&this->buffer_pool_mutex_);

  // Find the buffer in the pool and mark as free
  for (size_t i = 0; i < MAX_BUFFER_SLOTS; i++) {
    if (this->buffer_pool_[i].ptr == buffer) {
      this->buffer_pool_[i].in_use = false;
      portEXIT_CRITICAL(&this->buffer_pool_mutex_);
      return;
    }
  }

  portEXIT_CRITICAL(&this->buffer_pool_mutex_);
  ESP_LOGW(TAG, "Attempted to free buffer not in pool: %p", buffer);
}

size_t Storage::get_max_buffer_size() const {
  if (!this->psram_available_) {
    return 0;
  }

  // All buffers are 64KB
  return 65536;
}

}  // namespace storage
}  // namespace esphome
