# Binary Storage Bugfix Session - Known Facts & TODO

**Session Date:** 2025-11-15
**Component:** `esphome/components/binary_storage/`

---

## Known Facts

### What We Built (Previous Session)
- Complete binary storage system with 7 device types:
  - I2C EEPROM, I2C FRAM
  - SPI Flash (with QSPI), SPI FRAM, SPI MRAM
  - OneWire EEPROM
  - LittleFS filesystem support
- ~9,000 lines C++ code
- ~500 lines Python code
- Full automation actions and YAML configuration
- Mode support: raw, littlefs, both

### Current Session Goals
- Bugfixing across multiple components/sessions
- Will require multiple compacts overall
- Need to track facts and todos across conversation resets

---

## Bugs to Fix

### ✅ FIXED: Issue #1 - Invalid Python code in binary_storage/__init__.py
**Location:** `esphome/components/binary_storage/__init__.py:344-394`

**Problems Found:**
1. Invalid RawExpression with string interpolation (line 351)
2. Invalid method call `register_with_storage_host(mount_path, LittleFSMount)` with 2 parameters (line 353)
3. Missing device node registration for `raw` and `both` modes
4. Not following proper registration patterns

**Root Cause:**
- Incorrect understanding of how binary_storage registers with storage_host
- Binary storage has TWO registration mechanisms:
  - **Device nodes** (`/dev/framX`) - for raw mode, registered from Python
  - **Mount points** (`/fram`) - for littlefs mode, registered from C++ in LittleFSMount::setup()
- Previous fix incorrectly removed device node registration

**Complete Fix Applied:**
1. ✅ Fixed LittleFS mount component ID generation using `cg.ID()` (lines 353-357)
2. ✅ Removed invalid 2-parameter `register_with_storage_host()` call
3. ✅ **Restored device node registration for raw/both modes** (lines 368-384)
   - Auto-generates device node paths like `/dev/fram0`, `/dev/eeprom1`
   - Calls `var.register_with_storage_host(device_node_path)` with correct 1 parameter
   - Only executes when mode is `raw` or `both`
4. ✅ Kept CORE.data storage for storage_host discovery (lines 386-394)

### ✅ FIXED: Issue #2 - Missing binary_storage platform in storage_host
**Location:** `esphome/components/storage_host/__init__.py:53-77`

**Problem Found:**
- storage_host didn't include `"binary_storage"` as a valid platform type
- MOUNT_SCHEMA validation rejected binary_storage mounts

**Fix Applied:**
1. ✅ Added `PLATFORM_BINARY_STORAGE = "binary_storage"` (line 60)
2. ✅ Added to MOUNT_SCHEMA validation list (line 74)

### ✅ FIXED: Issue #3 - C++ Compilation Errors
**Locations:** Multiple C++ files

**Problems Found:**
1. TAG redefinition across multiple .cpp files
2. Missing esp_littlefs.h header (IDF component not loaded)
3. Deprecated I2C API usage (writev instead of write)

**Fixes Applied:**

**3a. TAG Redefinition**
- **binary_storage.h:11** - Removed `static const char *const TAG` from header
- **binary_storage.cpp:15** - Added TAG definition to .cpp file
- Each component file (i2c_fram, i2c_eeprom, onewire_eeprom) already had their own TAG

**3b. LittleFS Component Loading**
- **binary_storage/__init__.py:297-299** - Added conditional IDF component loading
- Only loads for ESP-IDF builds (check `CORE.using_esp_idf`)
- Imports `add_idf_component` conditionally to avoid unused import warning
- Always loads component because littlefs_mount.cpp is compiled even for raw mode

**3c. Deprecated I2C API**
- **i2c_fram.cpp:31, 226, 236** - Changed `writev()` to `write()`
- **i2c_fram.cpp:99** - Changed `writev()` to `write()`
- **i2c_eeprom.cpp:120** - Changed `writev()` to `write()`
- **i2c_eeprom.cpp:231** - Changed `writev()` to `write()`
- The `write()` method is a convenience wrapper around `write_readv()` (the new API)

---

## TODO List

### Current Tasks
- [x] Fix binary_storage/__init__.py Python registration issues
- [x] Add device node registration for raw/both modes
- [x] Add PLATFORM_BINARY_STORAGE to storage_host
- [x] Fix C++ compilation errors (TAG, I2C API, LittleFS, bus headers)
- [x] Implement conditional bus loading based on device type
- [ ] Test compilation with user's test config
- [ ] Fix any remaining compilation errors

---

## Completed Tasks

### 2025-11-15 - Python Fixes
1. ✅ Created bugfix session tracker
2. ✅ Analyzed sd_mmc_card and usb_msc_host registration patterns
3. ✅ Identified issues in binary_storage/__init__.py
4. ✅ Fixed LittleFS mount ID generation (cg.ID instead of RawExpression)
5. ✅ Restored device node registration for raw/both modes
6. ✅ Added binary_storage platform to storage_host
7. ✅ Verified both registration paths (device nodes + mounts) work correctly

### 2025-11-15 - C++ Fixes (Round 1)
8. ✅ Fixed TAG redefinition (moved from header to .cpp)
9. ✅ Fixed esp_littlefs.h include (conditional IDF component loading)
10. ✅ Fixed deprecated I2C API (writev → write) in 6 locations
11. ✅ Removed unused esp32 import (linter warning)

### 2025-11-15 - C++ Fixes (Round 2) - Conditional Bus Loading
12. ✅ Removed AUTO_LOAD = ["spi", "i2c"] from __init__.py
13. ✅ Added conditional defines based on device type:
    - USE_BINARY_STORAGE_SPI for SPI devices
    - USE_BINARY_STORAGE_I2C for I2C devices
    - USE_BINARY_STORAGE_ONEWIRE for OneWire devices
14. ✅ Wrapped all bus-specific .cpp files with #ifdef guards:
    - spi_flash.cpp, spi_fram.cpp, spi_mram.cpp → #ifdef USE_BINARY_STORAGE_SPI
    - i2c_fram.cpp, i2c_eeprom.cpp → #ifdef USE_BINARY_STORAGE_I2C
    - onewire_eeprom.cpp → #ifdef USE_BINARY_STORAGE_ONEWIRE
15. ✅ Wrapped all bus-specific .h files with matching guards
16. ✅ Fixed LittleFSMount destructor (removed override keyword)
17. ✅ Fixed StorageHost incomplete type (included storage_host.h instead of forward declaration)

---

## Important Constraints (Reference)

1. **NEVER make changes without explicit permission** - DESCRIBE, EXPLAIN, ASK, WAIT, then act
2. **NEVER modify core infrastructure** - Only modify binary_storage component
3. **Always verify before code generation** - Check actual definitions, don't assume
4. **Update this tracker** - Keep facts and todos current throughout session

---

## Notes
- This tracker will be updated throughout the bugfixing session
- Preserve known facts across context resets
- Track all identified bugs and their fixes
