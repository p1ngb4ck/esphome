#pragma once
#include "esphome/core/defines.h"

#ifdef USE_ESP_IDF
#include "binary_storage.h"
#include "esphome/core/component.h"
#include "esphome/components/storage/storage.h"
#include "lfs.h"
#include <string>
#include <memory>
#include <dirent.h>

namespace esphome {
namespace binary_storage {

// Maximum number of simultaneously open files (kept low for MCU memory constraints)
static constexpr int LFS_VFS_MAX_FDS = 4;

// Forward declaration
class LittleFSMount;

/**
 * @brief VFS context structure for LittleFS
 *
 * Holds filesystem state and file descriptor table for VFS operations
 */
struct LfsVfsContext {
  lfs_t *lfs;            ///< LittleFS filesystem object
  lfs_config *cfg;       ///< LittleFS configuration
  LittleFSMount *mount;  ///< Parent mount object

  // File descriptor table
  lfs_file_t files[LFS_VFS_MAX_FDS];  ///< File handles
  bool fd_used[LFS_VFS_MAX_FDS];      ///< FD in-use flags
  char *fd_paths[LFS_VFS_MAX_FDS];    ///< Paths for fstat support
};

/**
 * @brief Directory handle wrapper for LittleFS
 */
struct LfsVfsDir {
  DIR vfs_dir;           ///< VFS DIR struct (must be first)
  lfs_dir_t lfs_dir;     ///< LittleFS directory handle
  struct dirent dirent;  ///< Current directory entry
  char *path;            ///< Directory path
};

/**
 * @brief LittleFS mount manager for binary storage devices
 *
 * Mounts BinaryStorage devices (FRAM, EEPROM, Flash) as LittleFS filesystems
 * in the ESP-IDF VFS (Virtual File System).
 *
 * Features:
 * - Auto-format on first mount (optional)
 * - Custom block device adapter for BinaryStorage
 * - Integrates with storage for unified access
 */
class LittleFSMount : public Component, public storage::MountSpaceProvider {
 public:
  LittleFSMount() = default;
  ~LittleFSMount();

  // Component lifecycle
  void setup() override;
  void loop() override {}
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 100.0f; }

  /**
   * @brief list files in the mounted filesystem
   */
  void list_files() const;

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

  /**
   * @brief Get filesystem space information
   *
   * @param total_bytes Output: total space in bytes
   * @param used_bytes Output: used space in bytes
   * @return true on success, false if not mounted
   */
  bool get_space_info(uint64_t &total_bytes, uint64_t &used_bytes) override;

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
  LfsVfsContext *vfs_context_{nullptr};          ///< Context for VFS operations

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
   * @brief Register with vfs
   */
  void register_with_vfs_();

  /**
   * @brief Register with storage
   */
  void register_with_storage_();
};

}  // namespace binary_storage
}  // namespace esphome

#endif  // USE_ESP_IDF
