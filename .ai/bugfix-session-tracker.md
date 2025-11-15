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

---

## TODO List

### Current Tasks
- [x] Fix binary_storage/__init__.py Python registration issues
- [x] Add device node registration for raw/both modes
- [x] Add PLATFORM_BINARY_STORAGE to storage_host
- [ ] Test compilation with user's test config
- [ ] Fix C++ errors (if any appear during compilation)

---

## Completed Tasks

### 2025-11-15
1. ✅ Created bugfix session tracker
2. ✅ Analyzed sd_mmc_card and usb_msc_host registration patterns
3. ✅ Identified issues in binary_storage/__init__.py
4. ✅ Fixed LittleFS mount ID generation (cg.ID instead of RawExpression)
5. ✅ Restored device node registration for raw/both modes
6. ✅ Added binary_storage platform to storage_host
7. ✅ Verified both registration paths (device nodes + mounts) work correctly

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
