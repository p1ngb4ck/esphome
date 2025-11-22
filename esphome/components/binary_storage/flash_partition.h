#pragma once

#include "esphome/core/defines.h"
#ifdef USE_BINARY_STORAGE_FLASH_PARTITION

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"

#if CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/spi_flash.h"
#elif CONFIG_IDF_TARGET_ESP32S2
#include "esp32s2/rom/spi_flash.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/spi_flash.h"
#elif CONFIG_IDF_TARGET_ESP32C3
#include "esp32c3/rom/spi_flash.h"
#elif CONFIG_IDF_TARGET_ESP32H2
#include "esp32h2/rom/spi_flash.h"
#elif CONFIG_IDF_TARGET_ESP8684
#include "esp8684/rom/spi_flash.h"
#elif __has_include("esp32/rom/spi_flash.h")
#include "esp32/rom/spi_flash.h"  //IDF 4
#else
#include "rom/spi_flash.h"  //IDF 3
#endif
#include <string>

#ifdef USE_ESP_IDF
#include "esp_littlefs.h"
#endif

#ifdef USE_STORAGE
#include "esphome/components/storage/storage_device.h"
#endif

namespace esphome {
namespace binary_storage {

/**
 * @brief LittleFS on internal flash partition
 *
 * Uses ESP-IDF's built-in LittleFS support to mount a flash partition.
 * This is simpler than the BinaryStorage approach and doesn't require
 * external memory devices.
 *
 * Partition must be defined in partition table with subtype=littlefs:
 * ```
 * storage, data, littlefs, 0x3D0000, 0x20000,
 * ```
 */
class FlashPartition : public Component
#ifdef USE_STORAGE
    ,
                       public storage::StorageDevice
#endif
{
 public:
  FlashPartition() = default;
  ~FlashPartition();

  // Component lifecycle
  void setup() override;
  void loop() override {}
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  //========================================================================
  // Configuration
  //========================================================================

  /**
   * @brief Set partition label
   *
   * @param label Partition label from partition table
   */
  void set_partition_label(const std::string &label) { this->partition_label_ = label; }

  /**
   * @brief Set mount path
   *
   * @param path VFS mount point (e.g., "/littlefs")
   */
  void set_mount_path(const std::string &path) { this->mount_path_ = path; }

  /**
   * @brief Set auto-format on mount failure
   *
   * @param format If true, format filesystem if mount fails
   */
  void set_auto_format(bool format) { this->auto_format_ = format; }

  /**
   * @brief Set storage ID for registry
   *
   * @param id Storage identifier
   */
  void set_storage_id(const std::string &id) { this->storage_id_ = id; }

  /**
   * @brief Set human-readable name
   *
   * @param name Storage name
   */
  void set_storage_name(const std::string &name) { this->storage_name_ = name; }

  //========================================================================
  // Mount Management
  //========================================================================

  /**
   * @brief Check if filesystem is mounted
   *
   * @return true if mounted
   */
  bool is_mounted() const { return this->mounted_; }

  /**
   * @brief Get mount path
   *
   * @return Mount path string
   */
  const std::string &get_mount_path() const { return this->mount_path_; }

  /**
   * @brief Unmount filesystem
   *
   * @return true on success
   */
  bool unmount();

  /**
   * @brief Remount filesystem
   *
   * @return true on success
   */
  bool remount();

  /**
   * @brief Format the filesystem
   *
   * WARNING: Destroys all data!
   *
   * @return true on success
   */
  bool format();

#ifdef USE_STORAGE
  //========================================================================
  // StorageDevice Interface
  //========================================================================

  storage::StorageInfo get_info() override;
  bool is_available() override { return this->mounted_; }

  // No raw access for partition-based storage
  bool supports_raw_access() override { return false; }

  // Filesystem access
  bool supports_filesystem() override { return true; }
  std::string get_mount_path() override { return this->mount_path_; }

  // File operations
  bool file_exists(const char *path) override;
  bool get_file_size(const char *path, size_t *size) override;
  bool read_file(const char *path, uint8_t *data, size_t *length) override;
  bool write_file(const char *path, const uint8_t *data, size_t length) override;
  bool append_file(const char *path, const uint8_t *data, size_t length) override;
  bool delete_file(const char *path) override;
  bool rename_file(const char *old_path, const char *new_path) override;
  bool copy_file(const char *src_path, const char *dst_path) override;

  // Directory operations
  bool dir_exists(const char *path) override;
  bool create_dir(const char *path) override;
  bool delete_dir(const char *path, bool recursive) override;
  bool list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) override;

  // Space information
  bool get_space_info(uint64_t *total, uint64_t *free) override;
  bool can_write_file(const char *path, size_t size) override;

  // Streaming file access
  void *open_file(const char *path, const char *mode) override;
  size_t read_file_chunk(void *handle, uint8_t *buffer, size_t size) override;
  size_t write_file_chunk(void *handle, const uint8_t *data, size_t size) override;
  bool seek_file(void *handle, size_t offset) override;
  size_t tell_file(void *handle) override;
  bool close_file(void *handle) override;
#endif

 protected:
  std::string partition_label_;
  std::string mount_path_{"/littlefs"};
  std::string storage_id_;
  std::string storage_name_;
  bool auto_format_{true};
  bool mounted_{false};

  /**
   * @brief Build full VFS path from relative path
   */
  std::string build_path_(const char *path) const;
};

}  // namespace binary_storage
}  // namespace esphome

#endif  // USE_BINARY_STORAGE_FLASH_PARTITION
