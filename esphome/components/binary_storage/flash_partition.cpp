#include "esphome/core/defines.h"
#include "flash_partition.h"

#ifdef USE_BINARY_STORAGE_FLASH_PARTITION

#include "esphome/core/log.h"
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

namespace esphome {
namespace binary_storage {

static const char *const TAG = "flash_partition";

FlashPartition::~FlashPartition() {
  if (this->mounted_) {
    this->unmount();
  }
}

void FlashPartition::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LittleFS partition '%s'...", this->partition_label_.c_str());

#ifdef USE_ESP_IDF
  esp_vfs_littlefs_conf_t conf = {
      .base_path = this->mount_path_.c_str(),
      .partition_label = this->partition_label_.c_str(),
      .format_if_mount_failed = this->auto_format_,
      .dont_mount = false,
  };

  esp_err_t ret = esp_vfs_littlefs_register(&conf);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount or format filesystem");
    } else if (ret == ESP_ERR_NOT_FOUND) {
      ESP_LOGE(TAG, "Failed to find partition '%s'", this->partition_label_.c_str());
    } else {
      ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
    }
    this->mark_failed();
    return;
  }

  this->mounted_ = true;
  ESP_LOGI(TAG, "LittleFS mounted at '%s'", this->mount_path_.c_str());

  // Log filesystem info
  size_t total = 0, used = 0;
  ret = esp_littlefs_info(this->partition_label_.c_str(), &total, &used);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Partition size: total=%d, used=%d", total, used);
  }

#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_mount(this->mount_path_, "flash_partition", this);
    ESP_LOGD(TAG, "Registered mount with storage");
  }
#endif

#else
  ESP_LOGE(TAG, "FlashPartition requires ESP-IDF framework");
  this->mark_failed();
#endif
}

void FlashPartition::dump_config() {
  ESP_LOGCONFIG(TAG, "LittleFS Flash Partition:");
  ESP_LOGCONFIG(TAG, "  Partition: %s", this->partition_label_.c_str());
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->mount_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Auto format: %s", YESNO(this->auto_format_));
  ESP_LOGCONFIG(TAG, "  Mounted: %s", YESNO(this->mounted_));

#ifdef USE_ESP_IDF
  if (this->mounted_) {
    size_t total = 0, used = 0;
    if (esp_littlefs_info(this->partition_label_.c_str(), &total, &used) == ESP_OK) {
      ESP_LOGCONFIG(TAG, "  Total: %u bytes", total);
      ESP_LOGCONFIG(TAG, "  Used: %u bytes", used);
      ESP_LOGCONFIG(TAG, "  Free: %u bytes", total - used);
    }
  }
#endif
}

bool FlashPartition::unmount() {
#ifdef USE_ESP_IDF
  if (!this->mounted_) {
    return true;
  }

  esp_err_t ret = esp_vfs_littlefs_unregister(this->partition_label_.c_str());
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to unmount: %s", esp_err_to_name(ret));
    return false;
  }

  this->mounted_ = false;
  ESP_LOGI(TAG, "Unmounted '%s'", this->mount_path_.c_str());
  return true;
#else
  return false;
#endif
}

bool FlashPartition::remount() {
  if (this->mounted_) {
    if (!this->unmount()) {
      return false;
    }
  }

#ifdef USE_ESP_IDF
  esp_vfs_littlefs_conf_t conf = {
      .base_path = this->mount_path_.c_str(),
      .partition_label = this->partition_label_.c_str(),
      .format_if_mount_failed = false,
      .dont_mount = false,
  };

  esp_err_t ret = esp_vfs_littlefs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to remount: %s", esp_err_to_name(ret));
    return false;
  }

  this->mounted_ = true;
  return true;
#else
  return false;
#endif
}

bool FlashPartition::format() {
#ifdef USE_ESP_IDF
  bool was_mounted = this->mounted_;

  if (was_mounted) {
    if (!this->unmount()) {
      return false;
    }
  }

  esp_err_t ret = esp_littlefs_format(this->partition_label_.c_str());
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to format: %s", esp_err_to_name(ret));
    return false;
  }

  ESP_LOGI(TAG, "Formatted partition '%s'", this->partition_label_.c_str());

  if (was_mounted) {
    return this->remount();
  }

  return true;
#else
  return false;
#endif
}

std::string FlashPartition::build_path_(const char *path) const {
  std::string full_path = this->mount_path_;
  if (path[0] != '/') {
    full_path += '/';
  }
  full_path += path;
  return full_path;
}

#ifdef USE_STORAGE

storage::StorageInfo FlashPartition::get_info() {
  storage::StorageInfo info;
  info.id = this->storage_id_;
  info.name = this->storage_name_.empty() ? this->partition_label_ : this->storage_name_;
  info.type = storage::StorageType::BINARY_STORAGE;
  info.filesystem = storage::FilesystemType::LITTLEFS;
  info.mount_path = this->mount_path_;
  info.block_size = 4096;  // LittleFS block size on ESP32
  info.is_mounted = this->mounted_;
  info.is_removable = false;
  info.is_read_only = false;
  info.supports_raw_access = false;
  info.supports_filesystem = true;

#ifdef USE_ESP_IDF
  size_t total = 0, used = 0;
  if (esp_littlefs_info(this->partition_label_.c_str(), &total, &used) == ESP_OK) {
    info.total_bytes = total;
    info.free_bytes = total - used;
  }
#endif

  return info;
}

bool FlashPartition::file_exists(const char *path) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  struct stat st;
  return stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool FlashPartition::get_file_size(const char *path, size_t *size) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    return false;
  }
  *size = st.st_size;
  return true;
}

bool FlashPartition::read_file(const char *path, uint8_t *data, size_t *length) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  FILE *f = fopen(full_path.c_str(), "rb");
  if (!f) {
    return false;
  }

  size_t read = fread(data, 1, *length, f);
  fclose(f);
  *length = read;
  return true;
}

bool FlashPartition::write_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  FILE *f = fopen(full_path.c_str(), "wb");
  if (!f) {
    return false;
  }

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool FlashPartition::append_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  FILE *f = fopen(full_path.c_str(), "ab");
  if (!f) {
    return false;
  }

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool FlashPartition::delete_file(const char *path) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  return unlink(full_path.c_str()) == 0;
}

bool FlashPartition::rename_file(const char *old_path, const char *new_path) {
  if (!this->mounted_)
    return false;

  std::string old_full = this->build_path_(old_path);
  std::string new_full = this->build_path_(new_path);
  return rename(old_full.c_str(), new_full.c_str()) == 0;
}

bool FlashPartition::copy_file(const char *src_path, const char *dst_path) {
  if (!this->mounted_)
    return false;

  // Read source file
  size_t size;
  if (!this->get_file_size(src_path, &size)) {
    return false;
  }

  std::vector<uint8_t> buffer(size);
  size_t read_size = size;
  if (!this->read_file(src_path, buffer.data(), &read_size)) {
    return false;
  }

  // Write to destination
  return this->write_file(dst_path, buffer.data(), read_size);
}

bool FlashPartition::dir_exists(const char *path) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  struct stat st;
  return stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool FlashPartition::create_dir(const char *path) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  return mkdir(full_path.c_str(), 0755) == 0;
}

bool FlashPartition::delete_dir(const char *path, bool recursive) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);

  if (recursive) {
    // Delete contents first
    std::vector<storage::StorageFileInfo> entries;
    if (this->list_dir(path, &entries)) {
      for (const auto &entry : entries) {
        std::string entry_path = std::string(path) + "/" + entry.name;
        if (entry.is_directory) {
          this->delete_dir(entry_path.c_str(), true);
        } else {
          this->delete_file(entry_path.c_str());
        }
      }
    }
  }

  return rmdir(full_path.c_str()) == 0;
}

bool FlashPartition::list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) {
  if (!this->mounted_)
    return false;

  std::string full_path = this->build_path_(path);
  DIR *dir = opendir(full_path.c_str());
  if (!dir) {
    return false;
  }

  entries->clear();
  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    storage::StorageFileInfo info;
    info.name = ent->d_name;
    info.path = std::string(path) + "/" + ent->d_name;
    info.is_directory = (ent->d_type == DT_DIR);

    if (!info.is_directory) {
      std::string file_path = full_path + "/" + ent->d_name;
      struct stat st;
      if (stat(file_path.c_str(), &st) == 0) {
        info.size = st.st_size;
        info.modified_time = st.st_mtime;
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

bool FlashPartition::get_space_info(uint64_t *total, uint64_t *free) {
#ifdef USE_ESP_IDF
  if (!this->mounted_)
    return false;

  size_t total_size = 0, used_size = 0;
  esp_err_t ret = esp_littlefs_info(this->partition_label_.c_str(), &total_size, &used_size);
  if (ret != ESP_OK) {
    return false;
  }

  *total = total_size;
  *free = total_size - used_size;
  return true;
#else
  return false;
#endif
}

bool FlashPartition::get_space_info(uint64_t &total_bytes, uint64_t &used_bytes) {
#ifdef USE_ESP_IDF
  if (!this->mounted_)
    return false;

  size_t total_size = 0, used_size = 0;
  esp_err_t ret = esp_littlefs_info(this->partition_label_.c_str(), &total_size, &used_size);
  if (ret != ESP_OK) {
    return false;
  }

  total_bytes = total_size;
  used_bytes = used_size;
  return true;
#else
  return false;
#endif
}

bool FlashPartition::can_write_file(const char *path, size_t size) {
  uint64_t total, free_space;
  if (!this->get_space_info(&total, &free_space)) {
    return false;
  }
  return free_space >= size;
}

void *FlashPartition::open_file(const char *path, const char *mode) {
  if (!this->mounted_)
    return nullptr;

  std::string full_path = this->build_path_(path);
  return fopen(full_path.c_str(), mode);
}

size_t FlashPartition::read_file_chunk(void *handle, uint8_t *buffer, size_t size) {
  if (!handle)
    return 0;
  return fread(buffer, 1, size, static_cast<FILE *>(handle));
}

size_t FlashPartition::write_file_chunk(void *handle, const uint8_t *data, size_t size) {
  if (!handle)
    return 0;
  return fwrite(data, 1, size, static_cast<FILE *>(handle));
}

bool FlashPartition::seek_file(void *handle, size_t offset) {
  if (!handle)
    return false;
  return fseek(static_cast<FILE *>(handle), offset, SEEK_SET) == 0;
}

size_t FlashPartition::tell_file(void *handle) {
  if (!handle)
    return 0;
  return ftell(static_cast<FILE *>(handle));
}

bool FlashPartition::close_file(void *handle) {
  if (!handle)
    return false;
  return fclose(static_cast<FILE *>(handle)) == 0;
}

#endif  // USE_STORAGE

}  // namespace binary_storage
}  // namespace esphome

#endif  // USE_BINARY_STORAGE_FLASH_PARTITION
