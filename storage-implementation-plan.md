# Storage Architecture Implementation Plan

## Date: 2025-11-19
## Status: In Progress

---

## Overview

This document breaks the storage architecture implementation into discrete work blocks that can span multiple sessions. Each block leaves the codebase in a working state.

**Reference Documents:**
- `.ai/storage-architecture-design-internal.md` - Architecture decisions
- `.ai/binary-storage-complete-summary.md` - Binary storage features

---

## Current State Analysis

### Existing Components

1. **`storage_host`** - Already has registry-like functionality:
   - `StorageHost` class with mount management
   - `FileManager` with file watching and automation triggers
   - `NetworkStorage` interface
   - Global accessor `global_storage_host`

2. **`binary_storage`** - Complete with 6 device types:
   - I2C EEPROM, I2C FRAM
   - SPI Flash, SPI FRAM, SPI MRAM
   - OneWire EEPROM
   - LittleFS support via `littlefs_mount.cpp`

### Target State

- `storage_host` → `storage` (refactored)
- `binary_storage` implements `StorageDevice` interface
- `FileManager` remains as optional YAML-configurable tool in `storage`
- All existing configs continue to work

---

## Work Blocks

### WB-01: Create StorageDevice Interface in storage_host

**Goal:** Add the `StorageDevice` abstract interface to `storage_host` without breaking existing code.

**Files to Create:**
- `esphome/components/storage_host/storage_device.h` - Interface definition

**Files to Modify:**
- `esphome/components/storage_host/storage_host.h` - Include new header
- `esphome/components/storage_host/__init__.py` - Add `cg.add_define("USE_STORAGE")`

**Tasks:**
1. Create `storage_device.h` with:
   - `StorageType` enum
   - `FilesystemType` enum
   - `StorageInfo` struct
   - `FileInfo` struct (coordinate with existing `file_manager.h` FileInfo)
   - `StorageDevice` abstract class with all virtual methods

2. Update `storage_host.h`:
   - Include `storage_device.h`
   - Keep existing `StorageHost` class unchanged for now

3. Update `__init__.py`:
   - Add `cg.add_define("USE_STORAGE")` in `to_code()`

**Verification:**
- Existing `storage_host` configs still compile
- `USE_STORAGE` define is set when component is used

**Session Continuity:**
- Save this file path: `.ai/storage-implementation-plan.md`
- Current block: WB-01
- Block status: Not started

---

### WB-02: Add Registry Methods to StorageHost

**Goal:** Add device registration and query methods to `StorageHost` class.

**Prerequisites:** WB-01 completed

**Files to Modify:**
- `esphome/components/storage_host/storage_host.h` - Add registry methods
- `esphome/components/storage_host/storage_host.cpp` - Implement registry methods

**Tasks:**
1. Add to `StorageHost` class:
   ```cpp
   // Device registry
   void register_device(StorageDevice *device);
   void unregister_device(StorageDevice *device);
   void notify_device_changed(StorageDevice *device);

   // Query interface
   std::vector<StorageInfo> get_available_storages();
   std::vector<StorageDevice *> get_all_devices();
   StorageDevice *get_device_by_id(const std::string &id);
   StorageDevice *get_device_by_mount_path(const std::string &mount_path);

   // Callbacks
   void add_on_device_added_callback(std::function<void(StorageDevice *)> callback);
   void add_on_device_removed_callback(std::function<void(StorageDevice *)> callback);
   ```

2. Add protected member:
   ```cpp
   std::vector<StorageDevice *> devices_;
   CallbackManager<void(StorageDevice *)> on_device_added_callbacks_;
   CallbackManager<void(StorageDevice *)> on_device_removed_callbacks_;
   ```

3. Implement all new methods in `.cpp`

**Verification:**
- Existing configs still compile
- New methods are available but unused

**Session Continuity:**
- Current block: WB-02
- Block status: Not started

---

### WB-03: Add Streaming API to StorageDevice Interface

**Goal:** Add file handle-based streaming API for large files.

**Prerequisites:** WB-01 completed

**Files to Modify:**
- `esphome/components/storage_host/storage_device.h` - Add streaming methods

**Tasks:**
1. Add `FileHandle` struct or class
2. Add virtual methods:
   ```cpp
   virtual void *open_file(const char *path, const char *mode) { return nullptr; }
   virtual size_t read_file_chunk(void *handle, uint8_t *buffer, size_t size) { return 0; }
   virtual size_t write_file_chunk(void *handle, const uint8_t *data, size_t size) { return 0; }
   virtual bool seek_file(void *handle, size_t offset) { return false; }
   virtual size_t tell_file(void *handle) { return 0; }
   virtual bool close_file(void *handle) { return false; }
   ```

**Verification:**
- Interface compiles
- Default implementations return failure (safe fallback)

**Session Continuity:**
- Current block: WB-03
- Block status: Not started

---

### WB-04: Refactor binary_storage Base Class

**Goal:** Make `BinaryStorage` implement `StorageDevice` interface.

**Prerequisites:** WB-01 completed

**Files to Modify:**
- `esphome/components/binary_storage/binary_storage.h` - Inherit from StorageDevice
- `esphome/components/binary_storage/binary_storage.cpp` - Implement virtual methods
- `esphome/components/binary_storage/__init__.py` - Include storage_host if USE_STORAGE

**Tasks:**
1. Update `binary_storage.h`:
   ```cpp
   #ifdef USE_STORAGE
   #include "esphome/components/storage_host/storage_device.h"
   #endif

   class BinaryStorage : public Component
   #ifdef USE_STORAGE
     , public storage_host::StorageDevice
   #endif
   {
     // ... existing code ...

     #ifdef USE_STORAGE
     // StorageDevice interface implementations
     StorageInfo get_info() override;
     bool is_available() override;
     bool supports_raw_access() override { return true; }
     bool raw_read(uint32_t address, uint8_t *data, size_t length) override;
     bool raw_write(uint32_t address, const uint8_t *data, size_t length) override;
     // ... etc for filesystem methods if mode is littlefs
     #endif
   };
   ```

2. Implement the virtual methods using existing functionality

3. Add auto-registration in `setup()`:
   ```cpp
   #ifdef USE_STORAGE
   if (storage_host::global_storage_host != nullptr) {
     storage_host::global_storage_host->register_device(this);
   }
   #endif
   ```

**Verification:**
- `binary_storage` works standalone (without storage_host)
- `binary_storage` registers when `storage_host` is present
- All existing binary_storage configs compile

**Session Continuity:**
- Current block: WB-04
- Block status: Not started

---

### WB-05: Implement Filesystem Methods in binary_storage

**Goal:** Implement the filesystem virtual methods in BinaryStorage for LittleFS mode.

**Prerequisites:** WB-04 completed

**Files to Modify:**
- `esphome/components/binary_storage/binary_storage.h` - Add method declarations
- `esphome/components/binary_storage/binary_storage.cpp` - Implement methods

**Tasks:**
1. Implement filesystem methods that delegate to VFS when in LittleFS mode:
   - `file_exists()` - use POSIX `stat()`
   - `read_file()` - use POSIX `fopen/fread/fclose`
   - `write_file()` - use POSIX `fopen/fwrite/fclose`
   - `list_dir()` - use POSIX `opendir/readdir/closedir`
   - etc.

2. Return `false` for all filesystem methods when in raw mode

**Verification:**
- LittleFS mode devices can use filesystem methods
- Raw mode devices return false for filesystem methods

**Session Continuity:**
- Current block: WB-05
- Block status: Not started

---

### WB-06: Implement Streaming API in binary_storage

**Goal:** Implement file handle-based streaming for LittleFS mode.

**Prerequisites:** WB-05 completed

**Files to Modify:**
- `esphome/components/binary_storage/binary_storage.cpp`

**Tasks:**
1. Implement streaming methods using POSIX file handles:
   - `open_file()` - returns `FILE *` cast to `void *`
   - `read_file_chunk()` - uses `fread()`
   - `write_file_chunk()` - uses `fwrite()`
   - etc.

**Verification:**
- Can open, read chunks, write chunks, and close files
- Works for large files without loading entire file into memory

**Session Continuity:**
- Current block: WB-06
- Block status: Not started

---

### WB-07: Rename storage_host to storage

**Goal:** Rename the component while maintaining backward compatibility.

**Prerequisites:** WB-01 through WB-06 completed

**Files to Rename/Create:**
- `esphome/components/storage_host/` → `esphome/components/storage/`
- Keep `storage_host` as deprecated alias

**Tasks:**
1. Copy `storage_host/` to `storage/`
2. Update namespace from `storage_host` to `storage`
3. Update class names:
   - `StorageHost` → `Storage`
   - `global_storage_host` → `global_storage`
4. Update Python codegen
5. Create deprecated `storage_host` that forwards to `storage`:
   ```python
   # storage_host/__init__.py
   from esphome.components.storage import *
   _LOGGER.warning("'storage_host' is deprecated, use 'storage' instead")
   ```

**Verification:**
- Old `storage_host` configs work with deprecation warning
- New `storage` configs work
- `binary_storage` auto-registration works with new name

**Session Continuity:**
- Current block: WB-07
- Block status: Not started

---

### WB-08: Update binary_storage to Use New storage Component

**Goal:** Update binary_storage references from storage_host to storage.

**Prerequisites:** WB-07 completed

**Files to Modify:**
- `esphome/components/binary_storage/binary_storage.h`
- `esphome/components/binary_storage/binary_storage.cpp`
- `esphome/components/binary_storage/__init__.py`

**Tasks:**
1. Change includes from `storage_host/` to `storage/`
2. Change namespace from `storage_host` to `storage`
3. Change `global_storage_host` to `global_storage`

**Verification:**
- All binary_storage configs compile
- Auto-registration works with new storage component

**Session Continuity:**
- Current block: WB-08
- Block status: Not started

---

### WB-09: Add YAML Automation Triggers to storage

**Goal:** Add device_added/removed automation triggers to storage component.

**Prerequisites:** WB-07 completed

**Files to Modify:**
- `esphome/components/storage/__init__.py` - Add trigger schemas
- `esphome/components/storage/storage.h` - Add trigger classes

**Tasks:**
1. Add trigger classes:
   ```cpp
   class DeviceAddedTrigger : public Trigger<StorageDevice *> { ... };
   class DeviceRemovedTrigger : public Trigger<StorageDevice *> { ... };
   ```

2. Add YAML schema:
   ```python
   CONF_ON_DEVICE_ADDED = "on_device_added"
   CONF_ON_DEVICE_REMOVED = "on_device_removed"
   ```

3. Wire up in codegen

**Verification:**
- Can use `on_device_added` and `on_device_removed` in YAML
- Triggers fire when devices register/unregister

**Session Continuity:**
- Current block: WB-09
- Block status: Not started

---

### WB-10: Fork esp_littlefs Library

**Goal:** Create fork with custom block device support.

**Prerequisites:** None (can be done in parallel)

**External Work:**
1. Fork `joltwallet/esp_littlefs` to `esphome/esp_littlefs`
2. Add upstream `lfs.h` and `lfs.c`
3. Add `#ifdef USE_LITTLEFS_CUSTOM_BLOCK_DEVICE` guards

**Files to Modify (after fork created):**
- `esphome/components/binary_storage/__init__.py` - Use forked library

**Verification:**
- LittleFS mount works with both partition and custom block devices
- No binary size increase when not using custom block device

**Session Continuity:**
- Current block: WB-10
- Block status: Not started

---

## Future Work Blocks (After Core Architecture)

### WB-11: sd_mmc_card → sd_storage
Implement StorageDevice interface for SD cards.

### WB-12: usb_msc_host → usb_storage
Implement StorageDevice interface for USB mass storage.

### WB-13: http_file_server → http_file_browser
Update to use storage registry for device discovery.

### WB-14: webdav_server
Implement WebDAV using storage interface.

---

## Session Resume Instructions

When resuming work on this implementation:

1. **Read this file first:** `.ai/storage-implementation-plan.md`
2. **Check current block status** in this file
3. **Review architecture:** `.ai/storage-architecture-design-internal.md`
4. **Continue from current block** or ask user which block to work on

**Critical Rules:**
- Each block must leave codebase in working state
- Ask permission before making any changes
- Verify code compiles after each change
- Update "Block status" when starting/completing blocks

---

## Progress Tracking

| Block | Description | Status | Date |
|-------|-------------|--------|------|
| WB-01 | Create StorageDevice Interface | Not started | - |
| WB-02 | Add Registry Methods | Not started | - |
| WB-03 | Add Streaming API | Not started | - |
| WB-04 | Refactor binary_storage Base | Not started | - |
| WB-05 | Implement Filesystem Methods | Not started | - |
| WB-06 | Implement Streaming API | Not started | - |
| WB-07 | Rename storage_host → storage | Not started | - |
| WB-08 | Update binary_storage References | Not started | - |
| WB-09 | Add YAML Automation Triggers | Not started | - |
| WB-10 | Fork esp_littlefs Library | Not started | - |

---

## Notes

- FileManager stays in storage component (was in storage_host)
- NetworkStorage interface remains for NFS/SMB future support
- All existing mount/device_node functionality preserved
- Deprecation warnings for old names
