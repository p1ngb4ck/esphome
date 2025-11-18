#ifdef USE_ESP_IDF

#include "littlefs_mount.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "lfs.h"
#include "esp_vfs.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esphome/components/binary_storage/littlefs_mount.h"
// Soft dependency on storage_host
#if defined(USE_STORAGE_HOST)
#include "esphome/components/storage_host/storage_host.h"
#endif  // USE_STORAGE_HOST

namespace esphome {
namespace binary_storage {

static const char *const TAG = "littlefs_mount";

//========================================================================
// LittleFS Block Device Callbacks
//========================================================================

// Context passed to LittleFS callbacks
struct LittleFSContext {
  BinaryStorage *storage;
  BlockDeviceConfig config;
};

// Read a block
static int lfs_block_device_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer,
                                 lfs_size_t size) {
  auto *ctx = static_cast<LittleFSContext *>(c->context);
  int result = ctx->storage->block_read(block, off, buffer, size);
  return result == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

// Program (write) a block
static int lfs_block_device_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer,
                                 lfs_size_t size) {
  auto *ctx = static_cast<LittleFSContext *>(c->context);
  int result = ctx->storage->block_prog(block, off, buffer, size);
  return result == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

// Erase a block
static int lfs_block_device_erase(const struct lfs_config *c, lfs_block_t block) {
  auto *ctx = static_cast<LittleFSContext *>(c->context);
  int result = ctx->storage->block_erase(block);
  return result == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

// Sync the block device
static int lfs_block_device_sync(const struct lfs_config *c) {
  auto *ctx = static_cast<LittleFSContext *>(c->context);
  int result = ctx->storage->block_sync();
  return result == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

LittleFSMount::~LittleFSMount() {
  if (this->mounted_) {
    this->unmount();
  }

  // Free context
  if (this->lfs_context_ != nullptr) {
    delete static_cast<LittleFSContext *>(this->lfs_context_);
    this->lfs_context_ = nullptr;
  }
}

void LittleFSMount::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LittleFS Mount...");

  if (this->storage_ == nullptr) {
    ESP_LOGE(TAG, "No storage device configured!");
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "  Mount Path: %s", this->mount_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Storage Device: %s", this->storage_->get_device_name());
  ESP_LOGCONFIG(TAG, "  Auto Format: %s", this->auto_format_ ? "YES" : "NO");

  // Initialize LittleFS configuration
  if (!this->init_lfs_config_()) {
    ESP_LOGE(TAG, "Failed to initialize LittleFS configuration!");
    this->mark_failed();
    return;
  }

  // Attempt to mount
  if (this->mount_()) {
    ESP_LOGI(TAG, "Successfully mounted LittleFS at %s", this->mount_path_.c_str());
    this->register_with_storage_host_();
    this->register_with_vfs_();
  } else {
    ESP_LOGE(TAG, "Failed to mount LittleFS!");
    this->mark_failed();
  }
}

void LittleFSMount::dump_config() {
  ESP_LOGCONFIG(TAG, "LittleFS Mount:");
  ESP_LOGCONFIG(TAG, "  Mount Path: %s", this->mount_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Device: %s (%s)", this->storage_->get_device_name(), this->storage_->get_device_type());
  ESP_LOGCONFIG(TAG, "  Mounted: %s", this->mounted_ ? "YES" : "NO");

  if (this->mounted_ && this->lfs_ != nullptr) {
    // Get filesystem info
    lfs_ssize_t block_count = lfs_fs_size(this->lfs_.get());
    if (block_count >= 0) {
      uint32_t total_bytes = this->lfs_cfg_->block_count * this->lfs_cfg_->block_size;
      uint32_t used_bytes = block_count * this->lfs_cfg_->block_size;
      ESP_LOGCONFIG(TAG, "  Total: %u bytes (%.1f KB)", total_bytes, total_bytes / 1024.0f);
      ESP_LOGCONFIG(TAG, "  Used: %u bytes (%.1f KB, %.1f%%)", used_bytes, used_bytes / 1024.0f,
                    (used_bytes * 100.0f) / total_bytes);
      ESP_LOGCONFIG(TAG, "  Free: %u bytes (%.1f KB)", total_bytes - used_bytes, (total_bytes - used_bytes) / 1024.0f);
    }
  }
}

bool LittleFSMount::init_lfs_config_() {
  ESP_LOGD(TAG, "Initializing LittleFS configuration...");

  // Get block device configuration from storage
  BlockDeviceConfig block_config = this->storage_->get_block_config();

  ESP_LOGD(TAG, "Block device config: block_size=%u, block_count=%u, read_size=%u, prog_size=%u",
           block_config.block_size, block_config.block_count, block_config.read_size, block_config.prog_size);

  // Allocate LittleFS objects
  this->lfs_ = std::make_unique<lfs_t>();
  this->lfs_cfg_ = std::make_unique<lfs_config>();

  // Create context for callbacks
  auto *ctx = new LittleFSContext();
  ctx->storage = this->storage_;
  ctx->config = block_config;
  this->lfs_context_ = ctx;

  // Configure LittleFS
  memset(this->lfs_cfg_.get(), 0, sizeof(lfs_config));

  // Block device operations
  this->lfs_cfg_->read = lfs_block_device_read;
  this->lfs_cfg_->prog = lfs_block_device_prog;
  this->lfs_cfg_->erase = lfs_block_device_erase;
  this->lfs_cfg_->sync = lfs_block_device_sync;

  // Block device configuration
  this->lfs_cfg_->read_size = block_config.read_size;
  this->lfs_cfg_->prog_size = block_config.prog_size;
  this->lfs_cfg_->block_size = block_config.block_size;
  this->lfs_cfg_->block_count = block_config.block_count;
  this->lfs_cfg_->lookahead_size = block_config.lookahead_size;
  this->lfs_cfg_->cache_size = block_config.block_size;  // Cache size = block size
  this->lfs_cfg_->block_cycles = 500;                    // Wear leveling cycles

  // Allocate buffers
  this->read_buffer_ = std::make_unique<uint8_t[]>(block_config.block_size);
  this->prog_buffer_ = std::make_unique<uint8_t[]>(block_config.block_size);
  this->lookahead_buffer_ = std::make_unique<uint8_t[]>(block_config.lookahead_size);

  this->lfs_cfg_->read_buffer = this->read_buffer_.get();
  this->lfs_cfg_->prog_buffer = this->prog_buffer_.get();
  this->lfs_cfg_->lookahead_buffer = this->lookahead_buffer_.get();

  // Context for callbacks
  this->lfs_cfg_->context = this->lfs_context_;

  return true;
}

bool LittleFSMount::mount_() {
  ESP_LOGD(TAG, "Mounting LittleFS filesystem...");

  // Try to mount
  int err = lfs_mount(this->lfs_.get(), this->lfs_cfg_.get());

  if (err != LFS_ERR_OK) {
    if (this->auto_format_) {
      ESP_LOGW(TAG, "Mount failed (err=%d), attempting to format...", err);

      // Format the filesystem
      err = lfs_format(this->lfs_.get(), this->lfs_cfg_.get());
      if (err != LFS_ERR_OK) {
        ESP_LOGE(TAG, "Format failed (err=%d)", err);
        return false;
      }

      ESP_LOGI(TAG, "Format successful, mounting...");

      // Try to mount again
      err = lfs_mount(this->lfs_.get(), this->lfs_cfg_.get());
      if (err != LFS_ERR_OK) {
        ESP_LOGE(TAG, "Mount failed after format (err=%d)", err);
        return false;
      }
    } else {
      ESP_LOGE(TAG, "Mount failed (err=%d), auto_format disabled", err);
      return false;
    }
  }

  this->mounted_ = true;
  ESP_LOGI(TAG, "LittleFS mounted successfully");
  return true;
}

bool LittleFSMount::unmount() {
  if (!this->mounted_) {
    return true;
  }

  ESP_LOGD(TAG, "Unmounting LittleFS from %s...", this->mount_path_.c_str());

  int err = lfs_unmount(this->lfs_.get());

  if (err != LFS_ERR_OK) {
    ESP_LOGE(TAG, "Failed to unmount (err=%d)", err);
    return false;
  }

  this->mounted_ = false;
  return true;
}

bool LittleFSMount::remount() {
  if (this->mounted_) {
    if (!this->unmount()) {
      return false;
    }
  }

  return this->mount_();
}

bool LittleFSMount::format() {
  ESP_LOGW(TAG, "Formatting LittleFS filesystem - ALL DATA WILL BE LOST!");

  // Unmount first if mounted
  bool was_mounted = this->mounted_;
  if (was_mounted) {
    this->unmount();
  }

  int err = lfs_format(this->lfs_.get(), this->lfs_cfg_.get());

  if (err != LFS_ERR_OK) {
    ESP_LOGE(TAG, "Format failed (err=%d)", err);
    return false;
  }

  ESP_LOGI(TAG, "Format successful");

  // Remount if it was mounted before
  if (was_mounted) {
    return this->mount_();
  }

  return true;
}

void LittleFSMount::register_with_storage_host_() {
#if defined(USE_STORAGE_HOST)
  // Check if storage_host is available via global accessor (soft dependency)
  if (storage_host::global_storage_host != nullptr) {
    // Storage host exists, register this mount point
    std::string platform = this->storage_->get_device_type();
    storage_host::global_storage_host->register_mount(this->mount_path_, platform);
    ESP_LOGI(TAG, "Registered LittleFS mount with storage_host: %s (platform: %s)", this->mount_path_.c_str(),
             platform.c_str());
  } else {
    ESP_LOGD(TAG, "storage_host not available, mount will be standalone");
  }
#else
  ESP_LOGD(TAG, "storage_host component not compiled, mount registration disabled");
#endif  // USE_STORAGE_HOST
}

void LittleFSMount::register_with_vfs_() {
  esp_vfs_t vfs = {.flags = ESP_VFS_FLAG_DEFAULT,
                   .write = &lfs_write,
                   .open = &lfs_open,
                   .fstat = &lfs_fstat,
                   .close = &lfs_close,
                   .read = &lfs_read,
                   .lseek = &lfs_lseek,
                   .tell = &lfs_tell,
                   .mount_pt = this->mount_path_.c_str(),
                   .fs_data = this->lfs_.get()};

  esp_err_t err = esp_vfs_register(this->mount_path_.c_str(), &vfs, sizeof(vfs));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register LittleFS with VFS: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "LittleFS registered with VFS successfully");
  }
}

void LittleFSMount::list_files() const {
  if (!this->mounted_) {
    ESP_LOGW(TAG, "Filesystem not mounted, cannot list files");
    return;
  }

  ESP_LOGI(TAG, "Listing files in LittleFS at %s:", this->mount_path_.c_str());

  lfs_dir_t dir;
  int err = lfs_dir_open(this->lfs_.get(), &dir, "/");
  if (err != LFS_ERR_OK) {
    ESP_LOGE(TAG, "Failed to open root directory (err=%d)", err);
    return;
  }

  struct lfs_info info;
  while (true) {
    err = lfs_dir_read(this->lfs_.get(), &dir, &info);
    if (err < 0) {
      ESP_LOGE(TAG, "Failed to read directory (err=%d)", err);
      break;
    }
    if (err == 0) {
      // End of directory
      break;
    }

    if (info.type == LFS_TYPE_REG) {
      ESP_LOGI(TAG, "  File: %s, Size: %u", info.name, info.size);
    } else if (info.type == LFS_TYPE_DIR) {
      ESP_LOGI(TAG, "  Directory: %s", info.name);
    }
  }

  lfs_dir_close(this->lfs_.get(), &dir);
}

}  // namespace binary_storage
}  // namespace esphome

#endif  // USE_ESP_IDF
