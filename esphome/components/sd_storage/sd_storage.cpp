#include "sd_storage.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include <fstream>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <cstdio>
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

namespace esphome {
namespace sd_storage {

static sdmmc_host_t host = SDMMC_HOST_DEFAULT();
static sdmmc_card_t *card = nullptr;

void SdMmc::setup() {
  ESP_LOGI(TAG, "Initializing SD/MMC card");
  ESP_LOGI(TAG, "  CLK pin: %d, CMD pin: %d, DATA0 pin: %d", this->clk_pin_, this->cmd_pin_, this->data0_pin_);

  if (this->power_ctrl_pin_ != 0) {
    ESP_LOGI(TAG, "  Power control pin: %d", this->power_ctrl_pin_);
  }

  if (!this->mount_card()) {
    ESP_LOGE(TAG, "Failed to mount SD/MMC card");
    this->mark_failed();
    return;
  }

#ifdef USE_STORAGE
  // Register with storage registry if available (soft dependency)
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_device(this);
    ESP_LOGI(TAG, "Registered with storage registry");
  }
#endif
}

void SdMmc::loop() {
  // Nothing to do in loop
}

void SdMmc::dump_config() {
  ESP_LOGCONFIG(TAG, "SD/MMC Card:");
  ESP_LOGCONFIG(TAG, "  Mounted: %s", this->is_mounted_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->mount_path_.c_str());

  if (this->is_mounted_) {
    ESP_LOGCONFIG(TAG, "  Card Type: %d", static_cast<uint8_t>(this->card_type_));
    ESP_LOGCONFIG(TAG, "  Total bytes: %" PRIu64, this->total_bytes_);
    ESP_LOGCONFIG(TAG, "  Used bytes: %" PRIu64, this->used_bytes_);
  }
}

bool SdMmc::mount_card() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0 + this->slot_;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 50MHz

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = this->mode_1bit_ ? 1 : 4;

  // Configure SDMMC host
  host.flags = SDMMC_HOST_FLAG_4BIT;
  if (this->mode_1bit_) {
    host.flags = SDMMC_HOST_FLAG_1BIT;
  }
  host.slot = this->slot_;

#ifdef SOC_SDMMC_USE_GPIO_MATRIX
  // Configure pins
  slot_config.clk = static_cast<gpio_num_t>(this->clk_pin_);
  slot_config.cmd = static_cast<gpio_num_t>(this->cmd_pin_);
  slot_config.d0 = static_cast<gpio_num_t>(this->data0_pin_);
  if (!this->mode_1bit_) {
    slot_config.d1 = static_cast<gpio_num_t>(this->data1_pin_);
    slot_config.d2 = static_cast<gpio_num_t>(this->data2_pin_);
    slot_config.d3 = static_cast<gpio_num_t>(this->data3_pin_);
  }
#endif

  // Enable internal pull-ups for SD card communication
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  // Initialize host
  ESP_LOGI(TAG, "Initializing SDMMC slot %d", this->slot_);
  esp_err_t ret = sdmmc_host_init_slot(host.slot, &slot_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init SDMMC slot %d: %s", this->slot_, esp_err_to_name(ret));
    return false;
  }

  // Mount filesystem with retry logic
  const char *mount_point = this->mount_path_.c_str();
  const esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 16,
      .allocation_unit_size = 256 * 1024,
  };

  // Allocate card structure
  card = (sdmmc_card_t *) malloc(sizeof(sdmmc_card_t));

  // Attempt to mount with retries
  ret = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; attempt++) {
    ESP_LOGI(TAG, "Mounting SD card on slot %d to '%s' (attempt %d/3)...", this->slot_, mount_point, attempt);
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "SD card mounted successfully on slot %d to '%s'!", this->slot_, mount_point);
      break;
    }
    ESP_LOGW(TAG, "Mount attempt %d failed: %s", attempt, esp_err_to_name(ret));
    vTaskDelay(pdMS_TO_TICKS(100));  // Wait 100ms between attempts
  }

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount filesystem to '%s': %s", mount_point, esp_err_to_name(ret));
    free(card);
    card = nullptr;
    return false;
  }

  // Determine card type
  if (card->is_mmc) {
    this->card_type_ = CardType::MMC;
  } else if (card->is_sdio) {
    this->card_type_ = CardType::SDIO;
  } else {
    // Check if it's SDHC/SDXC (High Capacity) by looking at OCR register
    if (card->ocr & (1 << 30)) {
      this->card_type_ = CardType::SDHC;
    } else {
      this->card_type_ = CardType::SDSC;
    }
  }

  this->is_mounted_ = true;
  this->update_card_info();

  ESP_LOGI(TAG, "SD/MMC card mounted successfully at %s", mount_point);

  // Notify all registered callbacks that mount is ready
  ESP_LOGI(TAG, "Notifying %zu mount ready callbacks for '%s'", this->mount_ready_callbacks_.size(), mount_point);
  for (const auto &callback : this->mount_ready_callbacks_) {
    callback(this->mount_path_);
  }

  return true;
}

void SdMmc::unmount_card() {
  if (this->is_mounted_ && card != nullptr) {
    ESP_LOGI(TAG, "Unmounting SD/MMC card from '%s'", this->mount_path_.c_str());

#ifdef USE_STORAGE
    // Unregister from storage registry if available
    if (storage::global_storage != nullptr) {
      storage::global_storage->unregister_device(this);
    }
#endif

    esp_vfs_fat_sdcard_unmount(this->mount_path_.c_str(), card);
    free(card);
    card = nullptr;
    this->is_mounted_ = false;
    ESP_LOGI(TAG, "SD/MMC card unmounted successfully");
  }
}

bool SdMmc::update_card_info() {
  if (!this->is_mounted_ || card == nullptr) {
    return false;
  }

  // Get card info
  this->total_bytes_ = (uint64_t) card->csd.capacity * card->csd.sector_size;

  // Get filesystem usage using statvfs
  struct statvfs stat_buf;
  if (statvfs(this->mount_path_.c_str(), &stat_buf) == 0) {
    uint64_t free_bytes = (uint64_t) stat_buf.f_bfree * stat_buf.f_bsize;
    this->used_bytes_ = this->total_bytes_ - free_bytes;
  } else {
    this->used_bytes_ = 0;
  }

  return true;
}

uint64_t SdMmc::get_free_bytes() const {
  struct statvfs stat_buf;
  if (statvfs(this->mount_path_.c_str(), &stat_buf) == 0) {
    return (uint64_t) stat_buf.f_bfree * stat_buf.f_bsize;
  }
  return 0;
}

std::string SdMmc::build_full_path(const char *path) {
  std::string full_path = this->mount_path_;
  if (path[0] != '/') {
    full_path += "/";
  }
  full_path += path;
  return full_path;
}

// Original string-based methods
bool SdMmc::write_file(const std::string &path, const std::string &data) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG, "Card not mounted, cannot write file");
    return false;
  }

  std::string full_path = this->mount_path_ + "/" + path;
  std::ofstream file(full_path, std::ios::binary);

  if (!file.is_open()) {
    ESP_LOGW(TAG, "Failed to open file for writing: %s", full_path.c_str());
    return false;
  }

  file.write(data.c_str(), data.length());
  file.close();

  ESP_LOGD(TAG, "Wrote %d bytes to %s", data.length(), full_path.c_str());
  return true;
}

bool SdMmc::append_file(const std::string &path, const std::string &data) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG, "Card not mounted, cannot append to file");
    return false;
  }

  std::string full_path = this->mount_path_ + "/" + path;
  std::ofstream file(full_path, std::ios::binary | std::ios::app);

  if (!file.is_open()) {
    ESP_LOGW(TAG, "Failed to open file for appending: %s", full_path.c_str());
    return false;
  }

  file.write(data.c_str(), data.length());
  file.close();

  ESP_LOGD(TAG, "Appended %d bytes to %s", data.length(), full_path.c_str());
  return true;
}

std::string SdMmc::read_file(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG, "Card not mounted, cannot read file");
    return "";
  }

  std::string full_path = this->mount_path_ + "/" + path;
  std::ifstream file(full_path, std::ios::binary);

  if (!file.is_open()) {
    ESP_LOGW(TAG, "Failed to open file for reading: %s", full_path.c_str());
    return "";
  }

  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  file.close();

  ESP_LOGD(TAG, "Read %d bytes from %s", content.length(), full_path.c_str());
  return content;
}

bool SdMmc::delete_file(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG, "Card not mounted, cannot delete file");
    return false;
  }

  std::string full_path = this->mount_path_ + "/" + path;

  if (remove(full_path.c_str()) == 0) {
    ESP_LOGD(TAG, "Deleted file: %s", full_path.c_str());
    return true;
  } else {
    ESP_LOGW(TAG, "Failed to delete file: %s", full_path.c_str());
    return false;
  }
}

bool SdMmc::create_directory(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG, "Card not mounted, cannot create directory");
    return false;
  }

  std::string full_path = this->mount_path_ + "/" + path;

  if (mkdir(full_path.c_str(), 0755) == 0) {
    ESP_LOGD(TAG, "Created directory: %s", full_path.c_str());
    return true;
  } else {
    ESP_LOGW(TAG, "Failed to create directory: %s (errno: %d)", full_path.c_str(), errno);
    return false;
  }
}

bool SdMmc::remove_directory(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG, "Card not mounted, cannot remove directory");
    return false;
  }

  std::string full_path = this->mount_path_ + "/" + path;

  if (rmdir(full_path.c_str()) == 0) {
    ESP_LOGD(TAG, "Removed directory: %s", full_path.c_str());
    return true;
  } else {
    ESP_LOGW(TAG, "Failed to remove directory: %s", full_path.c_str());
    return false;
  }
}

bool SdMmc::is_directory(const std::string &path) {
  if (!this->is_mounted_) {
    return false;
  }

  std::string full_path = this->mount_path_ + "/" + path;
  struct stat path_stat;

  if (stat(full_path.c_str(), &path_stat) != 0) {
    return false;
  }

  return S_ISDIR(path_stat.st_mode);
}

uint32_t SdMmc::file_size(const std::string &path) {
  if (!this->is_mounted_) {
    return 0;
  }

  std::string full_path = this->mount_path_ + "/" + path;
  struct stat path_stat;

  if (stat(full_path.c_str(), &path_stat) != 0) {
    return 0;
  }

  return path_stat.st_size;
}

std::vector<FileInfo> SdMmc::list_directory(const std::string &path) {
  std::vector<FileInfo> files;

  if (!this->is_mounted_) {
    ESP_LOGW(TAG, "Card not mounted, cannot list directory");
    return files;
  }

  std::string full_path = this->mount_path_ + "/" + path;
  DIR *dir = opendir(full_path.c_str());

  if (dir == nullptr) {
    ESP_LOGW(TAG, "Failed to open directory: %s (errno: %d)", full_path.c_str(), errno);
    return files;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] != '.') {  // Skip hidden files
      FileInfo info;
      info.path = entry->d_name;
      info.is_directory = entry->d_type == DT_DIR;

      if (info.is_directory) {
        info.size = 0;
      } else {
        info.size = file_size(path + "/" + entry->d_name);
      }

      files.push_back(info);
    }
  }

  closedir(dir);
  ESP_LOGD(TAG, "Listed %d entries in %s", files.size(), full_path.c_str());
  return files;
}

#ifdef USE_STORAGE
//========================================================================
// StorageDevice Interface Implementation
//========================================================================

storage::StorageInfo SdMmc::get_info() {
  storage::StorageInfo info;
  info.id = this->id_.empty() ? "sd_storage" : this->id_;
  info.name = "SD Card";
  info.type = storage::StorageType::SD_CARD;
  info.filesystem = storage::FilesystemType::FAT;
  info.mount_path = this->mount_path_;
  info.total_bytes = this->total_bytes_;
  info.free_bytes = this->get_free_bytes();
  info.block_size = card != nullptr ? card->csd.sector_size : 512;
  info.is_mounted = this->is_mounted_;
  info.is_removable = true;
  info.is_read_only = false;
  info.supports_raw_access = false;
  info.supports_filesystem = true;
  return info;
}

bool SdMmc::file_exists(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;
  return stat(full_path.c_str(), &path_stat) == 0 && S_ISREG(path_stat.st_mode);
}

bool SdMmc::get_file_size(const char *path, size_t *size) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;

  if (stat(full_path.c_str(), &path_stat) != 0) {
    return false;
  }

  *size = path_stat.st_size;
  return true;
}

bool SdMmc::read_file(const char *path, uint8_t *data, size_t *length) {
  if (!this->is_mounted_)
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

bool SdMmc::write_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  FILE *f = fopen(full_path.c_str(), "wb");
  if (f == nullptr)
    return false;

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool SdMmc::append_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  FILE *f = fopen(full_path.c_str(), "ab");
  if (f == nullptr)
    return false;

  size_t written = fwrite(data, 1, length, f);
  fclose(f);
  return written == length;
}

bool SdMmc::delete_file(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  return remove(full_path.c_str()) == 0;
}

bool SdMmc::rename_file(const char *old_path, const char *new_path) {
  if (!this->is_mounted_)
    return false;

  std::string full_old = this->build_full_path(old_path);
  std::string full_new = this->build_full_path(new_path);
  return rename(full_old.c_str(), full_new.c_str()) == 0;
}

bool SdMmc::copy_file(const char *src_path, const char *dst_path) {
  if (!this->is_mounted_)
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

bool SdMmc::dir_exists(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;
  return stat(full_path.c_str(), &path_stat) == 0 && S_ISDIR(path_stat.st_mode);
}

bool SdMmc::create_dir(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  return mkdir(full_path.c_str(), 0755) == 0;
}

bool SdMmc::delete_dir(const char *path, bool recursive) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);

  if (recursive) {
    // Delete contents first
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

bool SdMmc::list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) {
  if (!this->is_mounted_)
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

    // Get file size and modification time
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

bool SdMmc::get_space_info(uint64_t *total, uint64_t *free) {
  if (!this->is_mounted_)
    return false;

  struct statvfs stat_buf;
  if (statvfs(this->mount_path_.c_str(), &stat_buf) != 0)
    return false;

  *total = (uint64_t) stat_buf.f_blocks * stat_buf.f_bsize;
  *free = (uint64_t) stat_buf.f_bfree * stat_buf.f_bsize;
  return true;
}

bool SdMmc::can_write_file(const char *path, size_t size) {
  if (!this->is_mounted_)
    return false;

  uint64_t total, free_space;
  if (!this->get_space_info(&total, &free_space))
    return false;

  return free_space >= size;
}

void *SdMmc::open_file(const char *path, const char *mode) {
  if (!this->is_mounted_)
    return nullptr;

  std::string full_path = this->build_full_path(path);
  return fopen(full_path.c_str(), mode);
}

size_t SdMmc::read_file_chunk(void *handle, uint8_t *buffer, size_t size) {
  if (handle == nullptr)
    return 0;
  return fread(buffer, 1, size, static_cast<FILE *>(handle));
}

size_t SdMmc::write_file_chunk(void *handle, const uint8_t *data, size_t size) {
  if (handle == nullptr)
    return 0;
  return fwrite(data, 1, size, static_cast<FILE *>(handle));
}

bool SdMmc::seek_file(void *handle, size_t offset) {
  if (handle == nullptr)
    return false;
  return fseek(static_cast<FILE *>(handle), offset, SEEK_SET) == 0;
}

size_t SdMmc::tell_file(void *handle) {
  if (handle == nullptr)
    return 0;
  return ftell(static_cast<FILE *>(handle));
}

bool SdMmc::close_file(void *handle) {
  if (handle == nullptr)
    return false;
  return fclose(static_cast<FILE *>(handle)) == 0;
}

bool SdMmc::format() {
  // Not implemented - would need to unmount, format, and remount
  ESP_LOGW(TAG, "Format not implemented for SD cards");
  return false;
}

bool SdMmc::sync() {
  // FAT filesystem syncs on file close
  return true;
}

#endif  // USE_STORAGE

}  // namespace sd_storage
}  // namespace esphome
