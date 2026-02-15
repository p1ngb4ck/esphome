#include "sd_storage_spi.h"

#ifdef USE_SD_STORAGE_SPI

#include "esphome/core/log.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>

extern "C" {
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
}
#include "ff.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

#ifndef VFS_FAT_MOUNT_DEFAULT_CONFIG
#define VFS_FAT_MOUNT_DEFAULT_CONFIG() \
  { .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 0, .disk_status_check_enable = false, }
#endif  // VFS_FAT_MOUNT_DEFAULT_CONFIG

// SD OCR SDHC capability bit
int constexpr SD_OCR_SDHC_CAP = (1 << 30);

namespace esphome {
namespace sd_storage {

std::string SdSpi::build_full_path(const char *path) {
  std::string full_path = this->mount_path_;
  if (path[0] != '/') {
    full_path += "/";
  }
  full_path += path;
  return full_path;
}

void SdSpi::setup() {
  ESP_LOGI(TAG_SPI, "Initializing SD card in SPI mode");

  // Setup DATA1/DATA2 pins as pullup inputs if specified
  auto setup_input_pullup = [](GPIOPin *pin) {
    pin->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
    pin->setup();
  };
  if (this->data1_pin_ != nullptr)
    setup_input_pullup(this->data1_pin_);
  if (this->data2_pin_ != nullptr)
    setup_input_pullup(this->data2_pin_);

  // Initialize SPI bus
  this->spi_setup();

  if (!this->mount_card()) {
    ESP_LOGE(TAG_SPI, "Failed to mount SD card");
    this->mark_failed();
    return;
  }

  // Note: Storage registration happens via mount_ready_callback
  // registered by storage component during codegen. This allows
  // removable media to register/unregister dynamically.
}

void SdSpi::loop() {
  // Nothing to do in loop
}

void SdSpi::dump_config() {
  ESP_LOGCONFIG(TAG_SPI, "SD Storage (SPI):");
  ESP_LOGCONFIG(TAG_SPI, "  Mounted: %s", this->is_mounted_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG_SPI, "  Mount path: %s", this->mount_path_.c_str());
  ESP_LOGCONFIG(TAG_SPI, "  Mode 1 bit: %s", YESNO(this->mode_1bit_));
  ESP_LOGCONFIG(TAG_SPI, "  CS Pin: %d", spi::Utility::get_pin_no(this->cs_));

  if (this->is_mounted_) {
    ESP_LOGCONFIG(TAG_SPI, "  Card Type: %d", static_cast<uint8_t>(this->card_type_));
    ESP_LOGCONFIG(TAG_SPI, "  Total bytes: %" PRIu64, this->total_bytes_);
    ESP_LOGCONFIG(TAG_SPI, "  Used bytes: %" PRIu64, this->used_bytes_);
  }

  if (this->is_failed()) {
    ESP_LOGE(TAG_SPI, "Setup failed: %s", SdSpi::error_code_to_string(this->init_error_).c_str());
  }
}

std::string SdSpi::error_code_to_string(SdSpi::ErrorCode code) {
  switch (code) {
    case ErrorCode::ERR_MOUNT:
      return "Failed to mount card";
    case ErrorCode::ERR_NO_CARD:
      return "No card found";
    default:
      return "Unknown error";
  }
}

bool SdSpi::mount_card() {
  ESP_LOGI(TAG_SPI, "Mounting SD card via SPI");

  // Log SPI configuration for debugging
  ESP_LOGD(TAG_SPI, "SPI Interface: %d", this->spi_interface_);
  ESP_LOGD(TAG_SPI, "CS Pin: %d", spi::Utility::get_pin_no(this->cs_));

  // Configure VFS mount
  esp_vfs_fat_sdmmc_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 16;
  mount_config.allocation_unit_size = 256 * 1024;

  // Initialize SDSPI host
  const auto init_err = sdspi_host_init();
  if (init_err != ESP_OK) {
    ESP_LOGE(TAG_SPI, "Failed to init sdspi host: %s", esp_err_to_name(init_err));
    this->init_error_ = ErrorCode::ERR_MOUNT;
    return false;
  }
  ESP_LOGV(TAG_SPI, "SDSPI host initialized");

  // Configure SDSPI device with CS pin from SPIDevice
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.host_id = this->spi_interface_;
  slot_config.gpio_cs = static_cast<gpio_num_t>(spi::Utility::get_pin_no(this->cs_));

  ESP_LOGD(TAG_SPI, "slot_config.host_id: %d, slot_config.gpio_cs: %d", slot_config.host_id, slot_config.gpio_cs);

  // Configure SDSPI host
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = this->spi_interface_;

  ESP_LOGD(TAG_SPI, "host.slot: %d", host.slot);

  // Try mounting with different frequencies
  esp_err_t mount_error = ESP_OK;
  for (const auto freq_khz : {SDMMC_FREQ_DEFAULT, SDMMC_FREQ_PROBING}) {
    host.max_freq_khz = freq_khz;
    ESP_LOGV(TAG_SPI, "Attempting mount with frequency: %d kHz", host.max_freq_khz);

    mount_error = esp_vfs_fat_sdspi_mount(this->mount_path_.c_str(), &host, &slot_config, &mount_config, &this->card_);
    if (mount_error != ESP_ERR_INVALID_RESPONSE) {
      break;
    }
  }

  if (mount_error != ESP_OK) {
    ESP_LOGE(TAG_SPI, "Failed to mount FAT fs: %s", esp_err_to_name(mount_error));
    switch (mount_error) {
      case ESP_FAIL:
      case ESP_ERR_INVALID_CRC:
        this->init_error_ = ErrorCode::ERR_MOUNT;
        break;
      case ESP_ERR_TIMEOUT:
      default:
        this->init_error_ = ErrorCode::ERR_NO_CARD;
    }
    return false;
  }

  // Determine card type
  if (this->card_->is_mmc) {
    this->card_type_ = CardType::MMC;
  } else if (this->card_->is_sdio) {
    this->card_type_ = CardType::SDIO;
  } else {
    // Check if it's SDHC/SDXC (High Capacity) by looking at OCR register
    if (this->card_->ocr & SD_OCR_SDHC_CAP) {
      this->card_type_ = CardType::SDHC;
    } else {
      this->card_type_ = CardType::SDSC;
    }
  }

  this->is_mounted_ = true;
  this->update_card_info();

  ESP_LOGI(TAG_SPI, "SD card mounted successfully at %s", this->mount_path_.c_str());
  ESP_LOGI(TAG_SPI, "  Max frequency: %d kHz", this->card_->max_freq_khz);
  ESP_LOGI(TAG_SPI, "  Real frequency: %d kHz", this->card_->real_freq_khz);

  // Notify all registered callbacks that mount is ready
  ESP_LOGI(TAG_SPI, "Notifying %zu mount ready callbacks for '%s'", this->mount_ready_callbacks_.size(),
           this->mount_path_.c_str());
  for (const auto &callback : this->mount_ready_callbacks_) {
    callback(this->mount_path_);
  }

#ifdef USE_STORAGE
  // Register with storage registry
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_device(this);
    ESP_LOGD(TAG_SPI, "Registered with storage registry");
  }
#endif

  return true;
}

void SdSpi::unmount_card() {
  if (this->is_mounted_ && this->card_ != nullptr) {
    ESP_LOGI(TAG_SPI, "Unmounting SD card from '%s'", this->mount_path_.c_str());

#ifdef USE_STORAGE
    // Unregister from storage registry if available
    if (storage::global_storage != nullptr) {
      storage::global_storage->unregister_device(this);
    }
#endif

    esp_vfs_fat_sdcard_unmount(this->mount_path_.c_str(), this->card_);
    this->card_ = nullptr;
    this->is_mounted_ = false;

    // Deinitialize SDSPI host
    sdspi_host_deinit();

    ESP_LOGI(TAG_SPI, "SD card unmounted");
  }
}

bool SdSpi::update_card_info() {
  if (!this->is_mounted_ || this->card_ == nullptr) {
    return false;
  }

  // Get card info
  this->total_bytes_ = (uint64_t) this->card_->csd.capacity * this->card_->csd.sector_size;

  // Get filesystem usage using f_getfree
  uint64_t total, free_bytes;
  if (this->get_space_info(&total, &free_bytes)) {
    this->used_bytes_ = total - free_bytes;
  } else {
    this->used_bytes_ = 0;
  }

  return true;
}

uint64_t SdSpi::get_free_bytes() const {
  if (!this->is_mounted_)
    return 0;

  FATFS *fs;
  DWORD fre_clust;

  std::string path = this->mount_path_;
  if (path.back() != '/') {
    path += '/';
  }

  FRESULT res = f_getfree(path.c_str(), &fre_clust, &fs);
  if (res != FR_OK) {
    return 0;
  }

  DWORD fre_sect = fre_clust * fs->csize;
  return (uint64_t) fre_sect * fs->ssize;
}

// Original string-based methods
bool SdSpi::write_file(const std::string &path, const std::string &data) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG_SPI, "Card not mounted, cannot write file");
    return false;
  }

  std::string full_path = this->build_full_path(path.c_str());
  FILE *file = fopen(full_path.c_str(), "wb");
  if (file == nullptr) {
    ESP_LOGW(TAG_SPI, "Failed to open file for writing: %s", full_path.c_str());
    return false;
  }

  size_t written = fwrite(data.c_str(), 1, data.length(), file);
  fclose(file);

  if (written != data.length()) {
    ESP_LOGW(TAG_SPI, "Failed to write all data to file");
    return false;
  }

  ESP_LOGD(TAG_SPI, "Wrote %d bytes to %s", data.length(), full_path.c_str());
  return true;
}

bool SdSpi::append_file(const std::string &path, const std::string &data) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG_SPI, "Card not mounted, cannot append to file");
    return false;
  }

  std::string full_path = this->build_full_path(path.c_str());
  FILE *file = fopen(full_path.c_str(), "ab");
  if (file == nullptr) {
    ESP_LOGW(TAG_SPI, "Failed to open file for appending: %s", full_path.c_str());
    return false;
  }

  size_t written = fwrite(data.c_str(), 1, data.length(), file);
  fclose(file);

  if (written != data.length()) {
    ESP_LOGW(TAG_SPI, "Failed to append all data to file");
    return false;
  }

  ESP_LOGD(TAG_SPI, "Appended %d bytes to %s", data.length(), full_path.c_str());
  return true;
}

std::string SdSpi::read_file(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG_SPI, "Card not mounted, cannot read file");
    return "";
  }

  std::string full_path = this->build_full_path(path.c_str());
  FILE *file = fopen(full_path.c_str(), "rb");
  if (file == nullptr) {
    ESP_LOGW(TAG_SPI, "Failed to open file for reading: %s", full_path.c_str());
    return "";
  }

  // Get file size
  fseek(file, 0, SEEK_END);
  size_t file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  // Read file content
  std::string content;
  content.resize(file_size);
  size_t bytes_read = fread(&content[0], 1, file_size, file);
  fclose(file);

  if (bytes_read != file_size) {
    ESP_LOGW(TAG_SPI, "Failed to read entire file");
    return "";
  }

  ESP_LOGD(TAG_SPI, "Read %d bytes from %s", bytes_read, full_path.c_str());
  return content;
}

bool SdSpi::delete_file(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG_SPI, "Card not mounted, cannot delete file");
    return false;
  }

  std::string full_path = this->build_full_path(path.c_str());

  if (remove(full_path.c_str()) == 0) {
    ESP_LOGD(TAG_SPI, "Deleted file: %s", full_path.c_str());
    return true;
  } else {
    ESP_LOGW(TAG_SPI, "Failed to delete file: %s", full_path.c_str());
    return false;
  }
}

bool SdSpi::create_directory(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG_SPI, "Card not mounted, cannot create directory");
    return false;
  }

  std::string full_path = this->build_full_path(path.c_str());

  if (mkdir(full_path.c_str(), 0755) == 0) {
    ESP_LOGD(TAG_SPI, "Created directory: %s", full_path.c_str());
    return true;
  } else {
    ESP_LOGW(TAG_SPI, "Failed to create directory: %s (errno: %d)", full_path.c_str(), errno);
    return false;
  }
}

bool SdSpi::remove_directory(const std::string &path) {
  if (!this->is_mounted_) {
    ESP_LOGW(TAG_SPI, "Card not mounted, cannot remove directory");
    return false;
  }

  std::string full_path = this->build_full_path(path.c_str());

  if (rmdir(full_path.c_str()) == 0) {
    ESP_LOGD(TAG_SPI, "Removed directory: %s", full_path.c_str());
    return true;
  } else {
    ESP_LOGW(TAG_SPI, "Failed to remove directory: %s", full_path.c_str());
    return false;
  }
}

bool SdSpi::is_directory(const std::string &path) {
  if (!this->is_mounted_) {
    return false;
  }

  std::string full_path = this->build_full_path(path.c_str());
  struct stat path_stat;

  if (stat(full_path.c_str(), &path_stat) != 0) {
    return false;
  }

  return S_ISDIR(path_stat.st_mode);
}

uint32_t SdSpi::file_size(const std::string &path) {
  if (!this->is_mounted_) {
    return 0;
  }

  std::string full_path = this->build_full_path(path.c_str());
  struct stat path_stat;

  if (stat(full_path.c_str(), &path_stat) != 0) {
    return 0;
  }

  return path_stat.st_size;
}

std::vector<FileInfo> SdSpi::list_directory(const std::string &path) {
  std::vector<FileInfo> files;

  if (!this->is_mounted_) {
    ESP_LOGW(TAG_SPI, "Card not mounted, cannot list directory");
    return files;
  }

  std::string full_path = this->build_full_path(path.c_str());
  DIR *dir = opendir(full_path.c_str());

  if (dir == nullptr) {
    ESP_LOGW(TAG_SPI, "Failed to open directory: %s (errno: %d)", full_path.c_str(), errno);
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
  ESP_LOGD(TAG_SPI, "Listed %d entries in %s", files.size(), full_path.c_str());
  return files;
}

#ifdef USE_STORAGE
//========================================================================
// StorageDevice Interface Implementation
//========================================================================

storage::StorageInfo SdSpi::get_info() {
  storage::StorageInfo info;
  info.id = this->id_.empty() ? "sd_storage_spi" : this->id_;
  info.name = "SD Card (SPI)";
  info.type = storage::StorageType::SD_CARD;
  info.filesystem = storage::FilesystemType::FAT;
  info.mount_path = this->mount_path_;
  info.total_bytes = this->total_bytes_;
  info.free_bytes = this->get_free_bytes();
  info.block_size = this->card_ != nullptr ? this->card_->csd.sector_size : 512;
  info.is_mounted = this->is_mounted_;
  info.is_removable = true;
  info.is_read_only = false;
  info.supports_raw_access = false;
  info.supports_filesystem = true;
  return info;
}

bool SdSpi::file_exists(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;
  return stat(full_path.c_str(), &path_stat) == 0 && S_ISREG(path_stat.st_mode);
}

bool SdSpi::get_file_size(const char *path, size_t *size) {
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

bool SdSpi::read_file(const char *path, uint8_t *data, size_t *length) {
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

bool SdSpi::write_file(const char *path, const uint8_t *data, size_t length) {
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

bool SdSpi::append_file(const char *path, const uint8_t *data, size_t length) {
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

bool SdSpi::delete_file(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  return remove(full_path.c_str()) == 0;
}

bool SdSpi::rename_file(const char *old_path, const char *new_path) {
  if (!this->is_mounted_)
    return false;

  std::string full_old = this->build_full_path(old_path);
  std::string full_new = this->build_full_path(new_path);
  return rename(full_old.c_str(), full_new.c_str()) == 0;
}

bool SdSpi::copy_file(const char *src_path, const char *dst_path) {
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

bool SdSpi::dir_exists(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  struct stat path_stat;
  return stat(full_path.c_str(), &path_stat) == 0 && S_ISDIR(path_stat.st_mode);
}

bool SdSpi::create_dir(const char *path) {
  if (!this->is_mounted_)
    return false;

  std::string full_path = this->build_full_path(path);
  return mkdir(full_path.c_str(), 0755) == 0;
}

bool SdSpi::delete_dir(const char *path, bool recursive) {
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

bool SdSpi::list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) {
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

bool SdSpi::get_space_info(uint64_t *total, uint64_t *free) {
  if (!this->is_mounted_)
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
    ESP_LOGW(TAG_SPI, "Failed to get filesystem info: %d", res);
    return false;
  }

  // Calculate total and free bytes
  // Cast to uint64_t before multiplication to avoid overflow on large cards
  DWORD tot_sect = (fs->n_fatent - 2) * fs->csize;
  DWORD fre_sect = fre_clust * fs->csize;

  // Sector size is typically 512 bytes for SD cards
  *total = (uint64_t) tot_sect * fs->ssize;
  *free = (uint64_t) fre_sect * fs->ssize;
  return true;
}

bool SdSpi::can_write_file(const char *path, size_t size) {
  if (!this->is_mounted_)
    return false;

  uint64_t total, free_space;
  if (!this->get_space_info(&total, &free_space))
    return false;

  return free_space >= size;
}

void *SdSpi::open_file(const char *path, const char *mode) {
  if (!this->is_mounted_)
    return nullptr;

  std::string full_path = this->build_full_path(path);
  return fopen(full_path.c_str(), mode);
}

size_t SdSpi::read_file_chunk(void *handle, uint8_t *buffer, size_t size) {
  if (handle == nullptr)
    return 0;
  return fread(buffer, 1, size, static_cast<FILE *>(handle));
}

size_t SdSpi::write_file_chunk(void *handle, const uint8_t *data, size_t size) {
  if (handle == nullptr)
    return 0;
  return fwrite(data, 1, size, static_cast<FILE *>(handle));
}

bool SdSpi::seek_file(void *handle, size_t offset) {
  if (handle == nullptr)
    return false;
  return fseek(static_cast<FILE *>(handle), offset, SEEK_SET) == 0;
}

size_t SdSpi::tell_file(void *handle) {
  if (handle == nullptr)
    return 0;
  return ftell(static_cast<FILE *>(handle));
}

bool SdSpi::close_file(void *handle) {
  if (handle == nullptr)
    return false;
  return fclose(static_cast<FILE *>(handle)) == 0;
}

bool SdSpi::format() {
  // Not implemented - would need to unmount, format, and remount
  ESP_LOGW(TAG_SPI, "Format not implemented for SD cards");
  return false;
}

bool SdSpi::sync() {
  // FAT filesystem syncs on file close
  return true;
}

#endif  // USE_STORAGE

}  // namespace sd_storage
}  // namespace esphome

#endif  // USE_SD_STORAGE_SPI
