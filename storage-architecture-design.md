# ESPHome Storage Architecture Design Document

## Date: 2025-11-19
## Status: Draft for Core Dev Review

---

## 1. Executive Summary

This document proposes a unified storage architecture for ESPHome that:
- Provides a consistent interface for all storage types
- Allows storage-type components to work independently
- Enables optional registry for multi-storage and removable device scenarios
- Minimizes risk of breaking changes through well-defined interfaces

---

## 2. Problem Statement

### Current Situation
ESPHome lacks a unified approach to storage. Multiple storage-related components are in development:
- `binary_storage` (FRAM, EEPROM, Flash)
- `sd_mmc_card`
- `usb_msc_host`
- `http_file_server`
- `webdav_server`

### Challenges
1. **No common interface** - Each component implements storage differently
2. **Discovery problem** - 3rd party components can't discover available storage
3. **Runtime changes** - SD cards and USB devices can be hot-plugged
4. **Breaking changes risk** - Future additions may break existing configs
5. **Code duplication** - File operations reimplemented in each component

### Requirements from Core Devs
- Storage must be a top-level component
- All storage-type components must also be top-level
- Users must not face breaking changes due to storage development
- Other components must be able to use storage easily

---

## 3. Proposed Architecture

### 3.1 Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    storage (top-level)                       │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Registry (REAL methods)                             │    │
│  │  - register/unregister devices                       │    │
│  │  - get_available_storages()                          │    │
│  │  - get_device_by_id/path()                           │    │
│  │  - mount/unmount callbacks                           │    │
│  └─────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  StorageDevice Interface (VIRTUAL methods)           │    │
│  │  - Device info                                       │    │
│  │  - Raw/binary access methods                         │    │
│  │  - Filesystem access methods                         │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ implements interface
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│binary_storage│    │ sd_mmc_card  │    │ usb_msc_host │
│  (top-level) │    │  (top-level) │    │  (top-level) │
│              │    │              │    │              │
│ Implements:  │    │ Implements:  │    │ Implements:  │
│ - raw access │    │ - filesystem │    │ - filesystem │
│ - filesystem │    │              │    │              │
│   (optional) │    │              │    │              │
└──────────────┘    └──────────────┘    └──────────────┘
```

### 3.2 Key Design Principles

1. **Independence**: Each storage-type component works fully standalone
2. **Optional Registry**: `storage` component only needed for multi-device or discovery scenarios
3. **Interface Stability**: Virtual methods defined once, implementations vary
4. **Capability-Based**: Components implement only what they support

---

## 4. Interface Definitions

### 4.1 Storage Info Structure

```cpp
namespace esphome {
namespace storage {

enum class StorageType : uint8_t {
  BINARY_STORAGE,   // FRAM, EEPROM, Flash
  SD_CARD,          // SD/MMC cards
  USB_MSC,          // USB mass storage
  // Future: NFS, SMB, etc.
};

enum class FilesystemType : uint8_t {
  NONE,             // Raw access only
  FAT,              // FAT16/FAT32
  EXFAT,            // exFAT
  LITTLEFS,         // LittleFS
  // SPIFFS deprecated, not included
};

struct StorageInfo {
  std::string id;                    // Unique identifier
  std::string name;                  // Human-readable name
  StorageType type;                  // Type of storage
  FilesystemType filesystem;         // Filesystem type (NONE if raw only)
  std::string mount_path;            // VFS mount path (empty if raw only)
  uint64_t total_bytes;              // Total capacity
  uint64_t free_bytes;               // Available space
  uint32_t block_size;               // Block/sector size
  bool is_mounted;                   // Currently mounted
  bool is_removable;                 // Can be hot-plugged
  bool is_read_only;                 // Write-protected
  bool supports_raw_access;          // Has raw read/write
  bool supports_filesystem;          // Has filesystem operations
};

struct FileInfo {
  std::string name;                  // File/directory name
  std::string path;                  // Full path
  uint64_t size;                     // Size in bytes (0 for directories)
  bool is_directory;                 // Is a directory
  uint32_t modified_time;            // Last modification (Unix timestamp, 0 if unavailable)
};

}  // namespace storage
}  // namespace esphome
```

### 4.2 StorageDevice Abstract Interface

```cpp
namespace esphome {
namespace storage {

class StorageDevice {
 public:
  virtual ~StorageDevice() = default;

  //========================================================================
  // Device Information (ALL must implement)
  //========================================================================

  /// Get device information
  virtual StorageInfo get_info() = 0;

  /// Check if device is currently available/mounted
  virtual bool is_available() = 0;

  //========================================================================
  // Raw/Binary Access (implement if supports_raw_access)
  //========================================================================

  /// Check if device supports raw binary access
  virtual bool supports_raw_access() { return false; }

  /// Read binary data from address
  /// @param address Starting address
  /// @param data Buffer to store data
  /// @param length Number of bytes to read
  /// @return true on success
  virtual bool raw_read(uint32_t address, uint8_t *data, size_t length) { return false; }

  /// Write binary data to address
  /// @param address Starting address
  /// @param data Data to write
  /// @param length Number of bytes to write
  /// @return true on success
  virtual bool raw_write(uint32_t address, const uint8_t *data, size_t length) { return false; }

  /// Erase block at address (for Flash devices)
  /// @param address Address within block to erase
  /// @return true on success
  virtual bool raw_erase(uint32_t address) { return true; }

  /// Get raw storage capacity in bytes
  virtual uint32_t get_raw_capacity() { return 0; }

  //========================================================================
  // Filesystem Access (implement if supports_filesystem)
  //========================================================================

  /// Check if device supports filesystem operations
  virtual bool supports_filesystem() { return false; }

  /// Get VFS mount path (e.g., "/sd", "/fram")
  virtual std::string get_mount_path() { return ""; }

  //------------------------------------------------------------------------
  // File Operations
  //------------------------------------------------------------------------

  /// Check if file exists
  virtual bool file_exists(const char *path) { return false; }

  /// Get file size
  /// @param path File path
  /// @param size Output: file size in bytes
  /// @return true if file exists
  virtual bool get_file_size(const char *path, size_t *size) { return false; }

  /// Read entire file into buffer
  /// @param path File path
  /// @param data Buffer to store data
  /// @param length In: buffer size, Out: bytes read
  /// @return true on success
  virtual bool read_file(const char *path, uint8_t *data, size_t *length) { return false; }

  /// Write data to file (creates or overwrites)
  /// @param path File path
  /// @param data Data to write
  /// @param length Number of bytes
  /// @return true on success
  virtual bool write_file(const char *path, const uint8_t *data, size_t length) { return false; }

  /// Append data to file
  virtual bool append_file(const char *path, const uint8_t *data, size_t length) { return false; }

  /// Delete file
  virtual bool delete_file(const char *path) { return false; }

  /// Rename/move file
  virtual bool rename_file(const char *old_path, const char *new_path) { return false; }

  /// Copy file
  virtual bool copy_file(const char *src_path, const char *dst_path) { return false; }

  //------------------------------------------------------------------------
  // Directory Operations
  //------------------------------------------------------------------------

  /// Check if directory exists
  virtual bool dir_exists(const char *path) { return false; }

  /// Create directory
  virtual bool create_dir(const char *path) { return false; }

  /// Delete directory
  /// @param path Directory path
  /// @param recursive If true, delete contents first
  /// @return true on success
  virtual bool delete_dir(const char *path, bool recursive = false) { return false; }

  /// List directory contents
  /// @param path Directory path
  /// @param entries Output: list of files/directories
  /// @return true on success
  virtual bool list_dir(const char *path, std::vector<FileInfo> *entries) { return false; }

  //------------------------------------------------------------------------
  // Space Information
  //------------------------------------------------------------------------

  /// Get filesystem space info
  /// @param total Output: total bytes
  /// @param free Output: free bytes
  /// @return true on success
  virtual bool get_space_info(uint64_t *total, uint64_t *free) { return false; }

  /// Check if file can be written (path valid + enough space)
  /// @param path Target file path
  /// @param size Required size in bytes
  /// @return true if write is possible
  virtual bool can_write_file(const char *path, size_t size) { return false; }

  //------------------------------------------------------------------------
  // Maintenance
  //------------------------------------------------------------------------

  /// Format the storage (WARNING: destroys all data)
  virtual bool format() { return false; }

  /// Sync/flush pending writes
  virtual bool sync() { return true; }
};

}  // namespace storage
}  // namespace esphome
```

### 4.3 Storage Registry Component

```cpp
namespace esphome {
namespace storage {

class Storage : public Component {
 public:
  void setup() override;
  void dump_config() override;

  //========================================================================
  // Registry Management (called by storage-type components)
  //========================================================================

  /// Register a storage device with the registry
  void register_device(StorageDevice *device);

  /// Unregister a storage device (e.g., on USB disconnect)
  void unregister_device(StorageDevice *device);

  /// Notify that a device's status changed (mounted/unmounted)
  void notify_device_changed(StorageDevice *device);

  //========================================================================
  // Query Interface (called by 3rd party components)
  //========================================================================

  /// Get list of all currently available storage devices
  std::vector<StorageInfo> get_available_storages();

  /// Get all storage devices (including unavailable)
  std::vector<StorageDevice *> get_all_devices();

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
  void add_on_device_added_callback(std::function<void(StorageDevice *)> callback);

  /// Add callback for when device is removed
  void add_on_device_removed_callback(std::function<void(StorageDevice *)> callback);

  /// Add callback for mount/unmount events
  void add_on_mount_state_callback(std::function<void(StorageDevice *, bool mounted)> callback);

 protected:
  std::vector<StorageDevice *> devices_;
  std::vector<std::function<void(StorageDevice *)>> on_added_callbacks_;
  std::vector<std::function<void(StorageDevice *)>> on_removed_callbacks_;
  std::vector<std::function<void(StorageDevice *, bool)>> on_mount_callbacks_;
};

// Global accessor for other components
extern Storage *global_storage;

}  // namespace storage
}  // namespace esphome
```

---

## 5. Usage Scenarios

### 5.1 Single Storage Device (No Registry Needed)

```yaml
# User just has one FRAM - works standalone
binary_storage:
  - type: FRAM
    id: my_fram
    model: MB85RC256
    mode: littlefs
    mount_path: /fram
```

The user can use it directly in lambdas:
```cpp
id(my_fram)->write_file("/config.json", data, len);
```

### 5.2 Multiple Storage Devices with Registry

```yaml
# User has multiple storage + wants http_file_server
storage:
  id: storage_registry

binary_storage:
  - type: FRAM
    id: my_fram
    mode: littlefs
    mount_path: /fram

sd_mmc_card:
  id: my_sd
  mount_path: /sd

http_file_server:
  storage_id: storage_registry  # Can discover all storage
```

The http_file_server queries available storage:
```cpp
auto storages = id(storage_registry)->get_available_storages();
for (auto &info : storages) {
  ESP_LOGI(TAG, "Storage: %s at %s", info.name.c_str(), info.mount_path.c_str());
}
```

### 5.3 Removable Storage with Callbacks

```yaml
storage:
  id: storage_registry
  on_device_added:
    - logger.log: "Storage added!"
  on_device_removed:
    - logger.log: "Storage removed!"

usb_msc_host:
  id: usb_storage
  mount_path: /usb
```

### 5.4 Raw Binary Access

```yaml
binary_storage:
  - type: FRAM
    id: my_fram
    mode: raw  # No filesystem, just binary access
```

```cpp
uint32_t counter;
id(my_fram)->raw_read(0x0000, (uint8_t*)&counter, sizeof(counter));
counter++;
id(my_fram)->raw_write(0x0000, (uint8_t*)&counter, sizeof(counter));
```

---

## 6. Implementation Strategy

### 6.1 Component Dependencies

```
storage (defines interface, provides registry)
    │
    ├── binary_storage (implements StorageDevice)
    │   └── Depends on: i2c, spi (optional)
    │
    ├── sd_mmc_card (implements StorageDevice)
    │   └── Depends on: (platform-specific)
    │
    └── usb_msc_host (implements StorageDevice)
        └── Depends on: usb_host
```

### 6.2 Soft Dependencies

Storage-type components should work WITHOUT the `storage` registry:
- Check if `global_storage != nullptr` before registering
- All functionality works standalone
- Registry is purely optional enhancement

```cpp
void BinaryStorageDevice::setup() {
  // ... device setup ...

  // Soft dependency: register with storage if available
  #ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_device(this);
  }
  #endif
}
```

### 6.3 PR Strategy

**Order of PRs:**

1. **`storage`** - Interface definitions + registry (can be empty initially)
2. **`binary_storage`** - Refactor to implement StorageDevice interface
3. **`sd_mmc_card`** - Implement StorageDevice interface
4. **`usb_msc_host`** - Implement StorageDevice interface
5. **`http_file_server`** - Use storage registry for discovery
6. **`webdav_server`** - Use storage registry

Each PR should be independently mergeable while maintaining backward compatibility.

---

## 7. Questions for Core Devs

### 7.1 Architecture

1. **Is this overall architecture acceptable?**
   - Top-level `storage` as registry + interface definition
   - Storage-type components as independent top-level components
   - Soft dependency pattern for optional registration

2. **Interface location**: ~~Should `StorageDevice` interface be in storage component or core?~~

   **Decided**: Interface goes in `esphome/components/storage/`. Storage-type components use soft dependency pattern - they include the interface header when `USE_STORAGE` is defined, allowing them to work independently when storage registry isn't used.

### 7.2 Naming

3. **Component names**:
   - `storage` - OK?
   - `binary_storage` vs `block_storage`?
   - `usb_msc_host` - OK or rename to `usb_storage`?
   - `http_file_server` → **`http_file_browser`** (proposed)

### 7.3 Libraries

4. **LittleFS library**:
   - Current: `joltwallet/esp_littlefs` only exposes `esp_littlefs.h` (partition-based)
   - We need raw `lfs.h` for custom block devices
   - Options:
     - A) Replace globally with upstream `littlefs`
     - B) `binary_storage` adds its own `littlefs` library
     - C) Fork `esp_littlefs` to expose `lfs.h`
   - Which approach is acceptable?

### 7.4 YAML Schema

5. **Registration syntax**: ~~Should storage-type components explicitly reference storage registry?~~

   **Decided**: Auto-registration (Option B) - storage-type components automatically register with `global_storage` if the `storage` component is present. Since there's only one registry, explicit IDs are unnecessary.

### 7.5 Filesystem Operations

6. **Path handling**: Should filesystem paths be:
   - Relative to device: `/myfile.txt`
   - Absolute with mount: `/sd/myfile.txt`
   - Both supported?

   **Proposed**: Support both via a wrapper that handles translation.

7. **Streaming API**: Do we need streaming file access for large files?
   ```cpp
   FileHandle *open_file(path, mode);
   read_chunk(handle, buffer, size);
   close_file(handle);
   ```
   Or is whole-file read/write sufficient?

   **Decided**: Yes, streaming API is needed for large files on memory-constrained MCUs.

---

## 8. Migration Path

### For Existing Users

If users already use `sd_mmc_card` or other storage components:
- Existing configs continue to work (standalone mode)
- Adding `storage:` component enables registry features
- No breaking changes to existing functionality

### For Component Developers

- Implement `StorageDevice` interface
- Register with `global_storage` if available (soft dependency)
- All existing functionality preserved

---

## 9. Future Considerations

### Potential Future Storage Types
- NFS mount
- SMB/CIFS mount
- HTTP/WebDAV mount (read-only)
- RAM disk

### Potential Future Features
- Storage quotas
- Access control
- Encryption layer

---

## 10. Summary

This architecture provides:

1. **Unified interface** - One way to interact with all storage types
2. **Independence** - Each component works standalone
3. **Discoverability** - 3rd party components can find available storage
4. **Stability** - Well-defined interface minimizes breaking changes
5. **Flexibility** - Supports raw access, filesystems, or both
6. **Runtime support** - Handles hot-pluggable devices

The key insight is separating:
- **Interface definition** (what methods exist) - stable, rarely changes
- **Registry** (what's currently available) - dynamic, runtime
- **Implementation** (how each storage type works) - varies per type

This separation allows independent development while maintaining compatibility.
