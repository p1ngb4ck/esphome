#pragma once

#ifdef USE_ESP_IDF

#include "binary_storage.h"
#include "esphome/core/component.h"
#include "littlefs/lfs.h"
#include <string>
#include <memory>

namespace esphome {
namespace binary_storage {

/**
 * @brief LittleFS mount manager for binary storage devices
 *
 * Mounts BinaryStorage devices (FRAM, EEPROM, Flash) as LittleFS filesystems
 * in the ESP-IDF VFS (Virtual File System).
 *
 * Features:
 * - Auto-format on first mount (optional)
 * - Custom block device adapter for BinaryStorage
 * - Integrates with storage_host for unified access
 */
class LittleFSMount : public Component {
 public:
  LittleFSMount() = default;
  ~LittleFSMount();

  // Component lifecycle
  void setup() override;
  void loop() override {}
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 100.0f; }

  //========================================================================
  // Configuration
  //========================================================================

  /**
   * @brief Set the binary storage device to mount
   *
   * @param storage Pointer to BinaryStorage device
   */
  void set_storage_device(BinaryStorage *storage) { this->storage_ = storage; }

  /**
   * @brief Set mount point path
   *
   * @param path VFS mount point (e.g., "/fram", "/eeprom")
   */
  void set_mount_path(const std::string &path) { this->mount_path_ = path; }

  /**
   * @brief Set whether to format on mount failure
   *
   * @param format If true, will format filesystem if mount fails
   */
  void set_auto_format(bool format) {
    ESP_LOGD("littlefs_mount", "set_auto_format called with: %s", format ? "true" : "false");
    this->auto_format_ = format;
  }

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
   * @brief Manually unmount filesystem
   *
   * @return true on success
   */
  bool unmount();

  /**
   * @brief Manually remount filesystem
   *
   * @return true on success
   */
  bool remount();

  /**
   * @brief Format the filesystem
   *
   * WARNING: This erases all data!
   *
   * @return true on success
   */
  bool format();

 protected:
  //========================================================================
  // Configuration
  //========================================================================

  BinaryStorage *storage_{nullptr};
  std::string mount_path_{"/storage"};
  bool auto_format_{true};
  bool mounted_{false};

  //========================================================================
  // LittleFS Objects
  //========================================================================

  std::unique_ptr<lfs_t> lfs_;                   ///< LittleFS filesystem object
  std::unique_ptr<lfs_config> lfs_cfg_;          ///< LittleFS configuration
  std::unique_ptr<uint8_t[]> read_buffer_;       ///< Read buffer for LittleFS
  std::unique_ptr<uint8_t[]> prog_buffer_;       ///< Program buffer for LittleFS
  std::unique_ptr<uint8_t[]> lookahead_buffer_;  ///< Lookahead buffer for LittleFS
  void *lfs_context_{nullptr};                   ///< Context for block device callbacks

  //========================================================================
  // Internal Helpers
  //========================================================================

  /**
   * @brief Mount the LittleFS filesystem
   *
   * @return true on success
   */
  bool mount_();

  /**
   * @brief Initialize LittleFS configuration
   *
   * @return true on success
   */
  bool init_lfs_config_();

  /**
   * @brief Register with storage_host
   */
  void register_with_storage_host_();
};

}  // namespace binary_storage
}  // namespace esphome

#endif  // USE_ESP_IDF
