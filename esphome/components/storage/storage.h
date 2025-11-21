#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstring>
#include <cstdint>
#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/core/optional.h"
#include "network_storage.h"
#include "storage_device.h"

namespace esphome {

// Forward declaration for binary_storage (soft dependency)
namespace binary_storage {
class BinaryStorage;
}

//  Forward declaration for network_storage (soft dependency)
namespace network_storage {
class NetworkStorage;
}

// Forward declaration for sd_storage (soft dependency)
namespace sd_storage {
class SdMmc;
}

// Forward declaration for usb_storage (soft dependency)
namespace usb_storage {
class USBStorageDevice;
}

namespace storage {

// Forward declarations
class Storage;

// Mount space info provider interface
class MountSpaceProvider {
 public:
  virtual ~MountSpaceProvider() = default;
  virtual bool get_space_info(uint64_t &total_bytes, uint64_t &used_bytes) = 0;
};

// Mount point entry - lightweight alternative to std::map
struct MountEntry {
  std::string path;
  std::string platform;
  MountSpaceProvider *space_provider{nullptr};  // Optional, for LittleFS etc.
};

// =====================================================
// StorageMount - Individual mount point reference
// =====================================================
class StorageMount {
 public:
  StorageMount() = default;

  void set_path(const std::string &path) { this->path_ = path; }
  void set_platform(const std::string &platform) { this->platform_ = platform; }
  void set_storage(Storage *storage) { this->storage_ = storage; }

  const std::string &get_path() const { return this->path_; }
  const std::string &get_platform() const { return this->platform_; }
  Storage *get_storage() const { return this->storage_; }

  /// Check if this mount point is available (e.g., SD card inserted)
  bool is_available() const;

  /// Get mount statistics (if supported by platform)
  bool get_stats(uint64_t &total_bytes, uint64_t &free_bytes) const;

 protected:
  std::string path_;
  std::string platform_;
  Storage *storage_{nullptr};
};

// Device node entry - virtual /dev files pointing to binary_storage devices
struct DeviceNode {
  std::string path;                       // e.g., "/dev/fram0"
  binary_storage::BinaryStorage *device;  // Pointer to binary_storage device
  std::string device_type;                // e.g., "i2c_fram", "spi_flash"
};

// Maximum number of mount points (SD, USB, internal, etc.)
static constexpr size_t MAX_MOUNT_POINTS = 8;

// Maximum number of device nodes (binary_storage devices in /dev)
static constexpr size_t MAX_DEVICE_NODES = 16;

// Maximum number of network storage backends (NFS, SMB, FTP, etc.)
static constexpr size_t MAX_NETWORK_STORAGE = 4;

// =====================================================
// Storage - Main Storage Class
// =====================================================
class Storage : public Component {
 public:
  Storage() = default;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // File methods
  bool file_exists(const std::string &path);
  std::string read_file(const std::string &path);
  bool write_file(const std::string &path, const std::string &data);
  std::vector<std::string> list_files(const std::string &path);

  // Mount management
  void register_mount(const std::string &path, const std::string &platform,
                      MountSpaceProvider *space_provider = nullptr);
  const esphome::StaticVector<MountEntry, MAX_MOUNT_POINTS> &get_mounts() const { return this->mounts_; }
  std::string find_mount_for_path(const std::string &path);

  // Device node management (virtual /dev files for binary_storage)
  void register_device_node(const std::string &path, binary_storage::BinaryStorage *device,
                            const std::string &device_type);
  const esphome::StaticVector<DeviceNode, MAX_DEVICE_NODES> &get_device_nodes() const { return this->device_nodes_; }
  bool is_device_node(const std::string &path) const;
  DeviceNode *find_device_node(const std::string &path);

  // Network storage management (NFS, SMB, FTP, etc.)
  void register_network_storage(NetworkStorage *storage);
  const esphome::StaticVector<NetworkStorage *, MAX_NETWORK_STORAGE> &get_network_storage() const {
    return this->network_storage_;
  }
  NetworkStorage *find_network_storage_for_path(const std::string &path);
  bool is_network_path(const std::string &path) const;

  // Network storage file operations (use network backend if available, fallback to POSIX)
  bool network_file_exists(const std::string &path);
  bool network_read_file(const std::string &path, std::vector<uint8_t> &data);
  bool network_write_file(const std::string &path, const uint8_t *data, size_t length);
  bool network_delete_file(const std::string &path);
  bool network_list_directory(const std::string &path, std::vector<NetworkStorage::DirEntry> &entries);
  bool network_create_directory(const std::string &path);
  bool network_delete_directory(const std::string &path);

  //========================================================================
  // StorageDevice Registry (for unified storage interface)
  //========================================================================

  /// Register a storage device with the registry
  void register_device(StorageDevice *device);

  /// Unregister a storage device (e.g., on USB disconnect)
  void unregister_device(StorageDevice *device);

  /// Notify that a device's status changed (mounted/unmounted)
  void notify_device_changed(StorageDevice *device);

  //========================================================================
  // Query Interface (for 3rd party components)
  //========================================================================

  /// Get list of all currently available storage devices
  std::vector<StorageInfo> get_available_storages();

  /// Get all storage devices (including unavailable)
  std::vector<StorageDevice *> get_all_devices() { return this->devices_; }

  /// Find device by unique ID
  StorageDevice *get_device_by_id(const std::string &id);

  /// Find device by mount path
  StorageDevice *get_device_by_mount_path(const std::string &mount_path);

  /// Find devices by type
  std::vector<StorageDevice *> get_devices_by_type(StorageType type);

  //========================================================================
  // Event Callbacks (for runtime changes)
  //========================================================================

  /// Add callback for when device becomes available
  void add_on_device_added_callback(std::function<void(StorageDevice *)> &&callback) {
    this->on_device_added_callbacks_.add(std::move(callback));
  }

  /// Add callback for when device is removed
  void add_on_device_removed_callback(std::function<void(StorageDevice *)> &&callback) {
    this->on_device_removed_callbacks_.add(std::move(callback));
  }

  /// Add callback for mount/unmount events
  void add_on_device_changed_callback(std::function<void(StorageDevice *)> &&callback) {
    this->on_device_changed_callbacks_.add(std::move(callback));
  }

  //========================================================================
  // Shared PSRAM Buffer Pool (for storage operations)
  //========================================================================

  /**
   * @brief Allocate a buffer from the shared PSRAM pool
   * @param size Requested size in bytes
   * @return Pointer to buffer, or nullptr if unavailable
   * @note Buffer is NOT automatically freed - caller must call free_buffer()
   * @note Thread-safe with mutex protection
   */
  uint8_t *allocate_buffer(size_t size);

  /**
   * @brief Free a buffer back to the pool
   * @param buffer Pointer returned by allocate_buffer()
   * @note Thread-safe with mutex protection
   */
  void free_buffer(uint8_t *buffer);

  /**
   * @brief Get maximum available buffer size in pool
   * @return Size in bytes, or 0 if no PSRAM
   */
  size_t get_max_buffer_size() const;

 protected:
  esphome::StaticVector<MountEntry, MAX_MOUNT_POINTS> mounts_;
  esphome::StaticVector<DeviceNode, MAX_DEVICE_NODES> device_nodes_;
  esphome::StaticVector<NetworkStorage *, MAX_NETWORK_STORAGE> network_storage_;

  // StorageDevice registry
  std::vector<StorageDevice *> devices_;
  CallbackManager<void(StorageDevice *)> on_device_added_callbacks_;
  CallbackManager<void(StorageDevice *)> on_device_removed_callbacks_;
  CallbackManager<void(StorageDevice *)> on_device_changed_callbacks_;

  // Shared PSRAM buffer pool
  struct BufferSlot {
    uint8_t *ptr{nullptr};
    size_t size{0};
    bool in_use{false};
  };
  static constexpr size_t MAX_BUFFER_SLOTS = 4;  // 4× 64KB = 256KB total
  BufferSlot buffer_pool_[MAX_BUFFER_SLOTS];
  portMUX_TYPE buffer_pool_mutex_ = portMUX_INITIALIZER_UNLOCKED;
  bool psram_available_{false};
};

// Global accessor for soft dependency pattern
extern Storage *global_storage;

}  // namespace storage
}  // namespace esphome
