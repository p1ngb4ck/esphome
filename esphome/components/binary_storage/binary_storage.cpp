#include "binary_storage.h"
#include "esphome/core/log.h"
#include "esphome/core/defines.h"
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

namespace esphome {
namespace binary_storage {

static const char *const TAG = "binary_storage";

void BinaryStorage::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Binary Storage...");
  ESP_LOGCONFIG(TAG, "  Device: %s", this->get_device_name());
  ESP_LOGCONFIG(TAG, "  Type: %s", this->get_device_type());
  ESP_LOGCONFIG(TAG, "  Capacity: %u bytes (%.1f KB)", this->get_capacity(), this->get_capacity() / 1024.0f);
  ESP_LOGCONFIG(TAG, "  Page Size: %u bytes", this->get_page_size());

  uint32_t erase_size = this->get_erase_size();
  if (erase_size > 0) {
    ESP_LOGCONFIG(TAG, "  Erase Block Size: %u bytes", erase_size);
  }

#ifdef USE_STORAGE
  // Auto-register with storage registry if available
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_device(this);
    ESP_LOGD(TAG, "Registered with storage registry: %s", this->storage_id_.c_str());
  }
#endif
}

void BinaryStorage::dump_config() {
  ESP_LOGCONFIG(TAG, "Binary Storage:");
  ESP_LOGCONFIG(TAG, "  Device: %s", this->get_device_name());
  ESP_LOGCONFIG(TAG, "  Capacity: %u bytes", this->get_capacity());
}

void BinaryStorage::register_with_storage(const std::string &device_node_path) {
#ifdef USE_STORAGE
  // Check if storage is available (soft dependency)
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_device_node(device_node_path, this, this->get_device_type());
    ESP_LOGI(TAG, "Registered device node: %s -> %s", device_node_path.c_str(), this->get_device_name());
  } else {
    ESP_LOGD(TAG, "storage not available, skipping device node registration");
  }
#else
  ESP_LOGD(TAG, "storage component not compiled, device node registration disabled");
#endif
}

uint32_t BinaryStorage::fill(uint8_t value) {
  const size_t buffer_size = std::min(static_cast<uint32_t>(256u), this->get_page_size() * 4);
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, value);

  uint32_t address = 0;
  uint32_t capacity = this->get_capacity();

  while (address < capacity) {
    size_t chunk_size = std::min((size_t) (capacity - address), buffer_size);
    if (!this->write(address, buffer, chunk_size)) {
      ESP_LOGE(TAG, "Fill failed at address 0x%X", address);
      return address;
    }
    address += chunk_size;
  }

  return capacity;
}

BlockDeviceConfig BinaryStorage::get_block_config() const {
  BlockDeviceConfig config;

  // Determine optimal block size based on device characteristics
  uint32_t page_size = this->get_page_size();
  uint32_t erase_size = this->get_erase_size();
  uint32_t capacity = this->get_capacity();

  // For Flash devices, use erase size as block size
  if (erase_size > 0) {
    config.block_size = erase_size;
  }
  // For EEPROM/FRAM, use a reasonable block size (4KB is common)
  else {
    config.block_size = 4096;
    // But don't exceed capacity
    if (config.block_size > capacity) {
      config.block_size = capacity;
    }
  }

  config.block_count = capacity / config.block_size;
  config.read_size = 1;                              // Can read single bytes
  config.prog_size = page_size > 0 ? page_size : 1;  // Program size is page size

  // Calculate lookahead_size: 1 bit per block, but must be multiple of 8
  // LittleFS requires lookahead_size to be divisible by 8
  uint32_t lookahead_bytes = (config.block_count + 7) / 8;
  config.lookahead_size = ((lookahead_bytes + 7) / 8) * 8;  // Round up to multiple of 8
  if (config.lookahead_size == 0) {
    config.lookahead_size = 8;  // Minimum size
  }

  return config;
}

int BinaryStorage::block_read(uint32_t block, uint32_t offset, void *buffer, uint32_t size) {
  BlockDeviceConfig cfg = this->get_block_config();
  uint32_t address = block * cfg.block_size + offset;

  if (!this->is_valid_address_(address, size)) {
    ESP_LOGE(TAG, "Block read out of bounds: block=%u, offset=%u, size=%u", block, offset, size);
    return -1;
  }

  return this->read(address, static_cast<uint8_t *>(buffer), size) ? 0 : -1;
}

int BinaryStorage::block_prog(uint32_t block, uint32_t offset, const void *buffer, uint32_t size) {
  BlockDeviceConfig cfg = this->get_block_config();
  uint32_t address = block * cfg.block_size + offset;

  if (!this->is_valid_address_(address, size)) {
    ESP_LOGE(TAG, "Block program out of bounds: block=%u, offset=%u, size=%u", block, offset, size);
    return -1;
  }

  return this->write(address, static_cast<const uint8_t *>(buffer), size) ? 0 : -1;
}

int BinaryStorage::block_erase(uint32_t block) {
  BlockDeviceConfig cfg = this->get_block_config();
  uint32_t address = block * cfg.block_size;

  return this->erase_block(address) ? 0 : -1;
}

#ifdef USE_STORAGE
// =====================================================
// StorageDevice Interface Implementation
// =====================================================

storage::StorageInfo BinaryStorage::get_info() {
  storage::StorageInfo info;
  info.id = this->storage_id_;
  info.name = this->storage_name_.empty() ? this->get_device_name() : this->storage_name_;
  info.type = storage::StorageType::BINARY_STORAGE;
  info.filesystem = this->filesystem_enabled_ ? storage::FilesystemType::LITTLEFS : storage::FilesystemType::NONE;
  info.mount_path = this->mount_path_;
  info.total_bytes = this->get_capacity();
  info.free_bytes = 0;  // Will be updated if filesystem is mounted
  info.block_size = this->get_page_size();
  info.is_mounted = this->filesystem_enabled_;  // Assume mounted if filesystem enabled
  info.is_removable = false;
  info.is_read_only = false;
  info.supports_raw_access = true;
  info.supports_filesystem = this->filesystem_enabled_;

  // Get actual free space if filesystem is enabled
  if (this->filesystem_enabled_) {
    uint64_t total, free_space;
    if (this->get_space_info(&total, &free_space)) {
      info.free_bytes = free_space;
    }
  }

  return info;
}

// File Operations - use POSIX VFS when filesystem is enabled
bool BinaryStorage::file_exists(const char *path) {
  if (!this->filesystem_enabled_)
    return false;
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool BinaryStorage::get_file_size(const char *path, size_t *size) {
  if (!this->filesystem_enabled_)
    return false;
  struct stat st;
  if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
    *size = st.st_size;
    return true;
  }
  return false;
}

bool BinaryStorage::read_file(const char *path, uint8_t *data, size_t *length) {
  if (!this->filesystem_enabled_)
    return false;

  FILE *f = fopen(path, "rb");
  if (f == nullptr)
    return false;

  size_t read = fread(data, 1, *length, f);
  fclose(f);
  *length = read;
  return true;
}

bool BinaryStorage::write_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->filesystem_enabled_)
    return false;

  FILE *f = fopen(path, "wb");
  if (f == nullptr)
    return false;

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool BinaryStorage::append_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->filesystem_enabled_)
    return false;

  FILE *f = fopen(path, "ab");
  if (f == nullptr)
    return false;

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool BinaryStorage::delete_file(const char *path) {
  if (!this->filesystem_enabled_)
    return false;
  return remove(path) == 0;
}

bool BinaryStorage::rename_file(const char *old_path, const char *new_path) {
  if (!this->filesystem_enabled_)
    return false;
  return rename(old_path, new_path) == 0;
}

bool BinaryStorage::copy_file(const char *src_path, const char *dst_path) {
  if (!this->filesystem_enabled_)
    return false;

  FILE *src = fopen(src_path, "rb");
  if (src == nullptr)
    return false;

  FILE *dst = fopen(dst_path, "wb");
  if (dst == nullptr) {
    fclose(src);
    return false;
  }

  uint8_t buffer[256];
  size_t bytes;
  bool success = true;

  while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
    if (fwrite(buffer, 1, bytes, dst) != bytes) {
      success = false;
      break;
    }
  }

  fclose(src);
  fclose(dst);
  return success;
}

// Directory Operations
bool BinaryStorage::dir_exists(const char *path) {
  if (!this->filesystem_enabled_)
    return false;
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool BinaryStorage::create_dir(const char *path) {
  if (!this->filesystem_enabled_)
    return false;
  return mkdir(path, 0755) == 0;
}

bool BinaryStorage::delete_dir(const char *path, bool recursive) {
  if (!this->filesystem_enabled_)
    return false;

  if (recursive) {
    // Delete contents first
    DIR *dir = opendir(path);
    if (dir == nullptr)
      return false;

    struct dirent *entry;
    char filepath[256];

    while ((entry = readdir(dir)) != nullptr) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        continue;

      snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);

      if (entry->d_type == DT_DIR) {
        delete_dir(filepath, true);
      } else {
        remove(filepath);
      }
    }
    closedir(dir);
  }

  return rmdir(path) == 0;
}

bool BinaryStorage::list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) {
  if (!this->filesystem_enabled_)
    return false;

  DIR *dir = opendir(path);
  if (dir == nullptr)
    return false;

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    storage::StorageFileInfo info;
    info.name = entry->d_name;
    info.path = std::string(path) + "/" + entry->d_name;
    info.is_directory = (entry->d_type == DT_DIR);

    // Get size for files
    if (!info.is_directory) {
      struct stat st;
      if (stat(info.path.c_str(), &st) == 0) {
        info.size = st.st_size;
        info.modified_time = st.st_mtime;
      } else {
        info.size = 0;
        info.modified_time = 0;
      }
    } else {
      info.size = 0;
      info.modified_time = 0;
    }

    entries->push_back(info);
  }

  closedir(dir);
  return true;
}

// Space Information
bool BinaryStorage::get_space_info(uint64_t *total, uint64_t *free) {
  if (!this->filesystem_enabled_)
    return false;

  // For LittleFS on custom block device, we need to use the mount path
  // This requires platform-specific implementation
  // For now, return capacity as total and estimate free
  *total = this->get_capacity();
  *free = *total / 2;  // Placeholder - actual implementation needs LittleFS API

  return true;
}

bool BinaryStorage::can_write_file(const char *path, size_t size) {
  if (!this->filesystem_enabled_)
    return false;

  uint64_t total, free_space;
  if (!this->get_space_info(&total, &free_space))
    return false;

  return free_space >= size;
}

// Streaming File Access
void *BinaryStorage::open_file(const char *path, const char *mode) {
  if (!this->filesystem_enabled_)
    return nullptr;
  return fopen(path, mode);
}

size_t BinaryStorage::read_file_chunk(void *handle, uint8_t *buffer, size_t size) {
  if (handle == nullptr)
    return 0;
  return fread(buffer, 1, size, static_cast<FILE *>(handle));
}

size_t BinaryStorage::write_file_chunk(void *handle, const uint8_t *data, size_t size) {
  if (handle == nullptr)
    return 0;
  return fwrite(data, 1, size, static_cast<FILE *>(handle));
}

bool BinaryStorage::seek_file(void *handle, size_t offset) {
  if (handle == nullptr)
    return false;
  return fseek(static_cast<FILE *>(handle), offset, SEEK_SET) == 0;
}

size_t BinaryStorage::tell_file(void *handle) {
  if (handle == nullptr)
    return 0;
  return ftell(static_cast<FILE *>(handle));
}

bool BinaryStorage::close_file(void *handle) {
  if (handle == nullptr)
    return false;
  return fclose(static_cast<FILE *>(handle)) == 0;
}

// Maintenance
bool BinaryStorage::format() {
  if (!this->filesystem_enabled_)
    return false;

  // For LittleFS, format needs to be called on the filesystem
  // This requires the LittleFS mount to be available
  // For now, just fill with 0xFF (erase pattern)
  this->fill(0xFF);
  return true;
}

#endif  // USE_STORAGE

}  // namespace binary_storage
}  // namespace esphome
