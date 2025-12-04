# SD Storage SPI Support Implementation Plan

## Overview
Add full SPI support to the `sd_storage` component while maintaining all existing storage integration features.

## Reference Repository Analysis
Source: https://github.com/asergunov/esphome_sd_card
Location: `/tmp/reference_sd_card/`

### Features to Import
1. **SPI Mode Support** - Add `type: sd_spi` alongside existing `type: sd_mmc`
2. **ESPHome SPI Integration** - Proper `spi::SPIDevice` inheritance and bus sharing
3. **Pin Mapping Differences**:
   - SDMMC: CLK, CMD, DATA0-3 pins
   - SPI: CLK→CLK, CMD→MOSI, DATA0→MISO, DATA3/CS→CS

### Features to Preserve (Unique to Our Implementation)
1. Full `StorageDevice` interface implementation
2. Mount/unmount callbacks for dynamic registration
3. Storage registry integration
4. `storage::global_storage` registration/unregistration

## Implementation Steps

### 1. Update Python Configuration (`__init__.py`)
- [ ] Create typed schema with `TYPE_SD_MMC` and `TYPE_SD_SPI`
- [ ] Add `SdSpi` class declaration that inherits from `spi::SPIDevice`
- [ ] Keep existing `SdMmc` class (rename if needed for clarity)
- [ ] Add SPI-specific schema with:
  - `spi_device_schema()` extension
  - CS pin configuration (can use `data3_pin` as alias)
  - Validation to ensure SPI bus pins are defined in SPI component
  - Optional `data1_pin` and `data2_pin` as pullup inputs (unused in SPI mode)
- [ ] Add SDMMC-specific schema (existing config)
- [ ] Add validators:
  - `validate_spi_cs_config()` - Handle CS pin vs data3_pin alias
  - `validate_spi_bus_pins()` - Ensure bus pins are in SPI config
  - `validate_spi_mode()` - Ensure ESP-IDF framework and 1-bit mode
  - `validate_config()` - Check platform compatibility (ESP32C6 needs SPI)
- [ ] Add final_validate to extract SPI interface index from SPI bus config
- [ ] Add defines: `USE_SD_STORAGE_SPI` and `USE_SD_STORAGE_SDMMC`
- [ ] Keep storage integration (VFS dir requirement, storage device registration)

### 2. Create C++ Header Files

#### `sd_storage.h` (Base/Common)
- [ ] Keep existing `FileInfo` struct
- [ ] Keep `CardType` enum
- [ ] Keep `mount_ready_callback_t` typedef
- [ ] Move common interface methods to base class or shared methods

#### `sd_storage_mmc.h` (SDMMC Mode - Existing)
- [ ] Wrap in `#ifdef USE_SD_STORAGE_SDMMC`
- [ ] Keep existing `SdMmc` class implementation
- [ ] Ensure it implements both `Component` and `StorageDevice`
- [ ] Keep all existing storage integration

#### `sd_storage_spi.h` (NEW - SPI Mode)
- [ ] Wrap in `#ifdef USE_SD_STORAGE_SPI`
- [ ] Create `SdSpi` class:
  ```cpp
  class SdSpi : public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST,
                                      spi::CLOCK_POLARITY_LOW,
                                      spi::CLOCK_PHASE_LEADING,
                                      spi::DATA_RATE_10MHZ>,
                public Component,
                public storage::StorageDevice
  ```
- [ ] Add SPI-specific members:
  - `SPIInterface spi_interface_`
  - `sdmmc_card_t *card_` (ESP-IDF)
  - Optional `data1_pin_`, `data2_pin_` (for pullup on unused pins)
- [ ] Keep all `StorageDevice` interface methods
- [ ] Add mount/unmount methods with storage registration

### 3. Create C++ Implementation Files

#### `sd_storage_spi.cpp` (NEW)
Based on `/tmp/reference_sd_card/components/sd_mmc_card/sd_spi_card_esp_idf.cpp`

- [ ] Implement `setup()`:
  - Call `spi_setup()` from `SPIDevice`
  - Initialize unused pins (data1, data2) as pullup inputs
  - Initialize SDSPI host with `sdspi_host_init()`
  - Configure `sdmmc_host_t` with SPI interface
  - Configure `sdspi_device_config_t` with CS pin and host_id
  - Mount with `esp_vfs_fat_sdspi_mount()`
  - Retry logic with different frequencies (DEFAULT, PROBING)
  - Determine card type from OCR register
  - Trigger mount_ready_callbacks
  - Register with storage::global_storage
- [ ] Implement `mount_card()` and `unmount_card()` with storage registration
- [ ] Implement all `StorageDevice` interface methods (file ops, dir ops, streaming)
- [ ] Use `build_full_path()` helper to prepend mount path
- [ ] Implement space info using `f_getfree()`

#### `sd_storage_mmc.cpp` (Update Existing `sd_storage.cpp`)
- [ ] Rename to clarify it's SDMMC mode
- [ ] Wrap in `#ifdef USE_SD_STORAGE_SDMMC`
- [ ] Keep all existing implementation
- [ ] No changes to logic needed

### 4. Integration Points

#### Storage Registration (Critical)
Both SPI and SDMMC modes must:
- [ ] Register with `storage::global_storage` after successful mount
- [ ] Unregister before unmount
- [ ] Trigger `mount_ready_callbacks` for consumers
- [ ] Implement full `StorageDevice` interface

#### VFS Directory Support
- [ ] Ensure `esp32::require_vfs_dir()` is called in Python codegen
- [ ] Works for both SPI and SDMMC modes

### 5. Testing Plan
- [ ] Test SDMMC mode (existing functionality)
- [ ] Test SPI mode with dedicated SPI bus
- [ ] Test SPI mode sharing bus with other SPI devices
- [ ] Test storage registry integration for both modes
- [ ] Test mount/unmount callbacks
- [ ] Test file operations through StorageDevice interface
- [ ] Test on ESP32, ESP32-S3, ESP32-C6

## Key Differences: Reference vs Our Implementation

| Feature | Reference Repo | Our Implementation |
|---------|---------------|-------------------|
| Storage Integration | ❌ None | ✅ Full StorageDevice interface |
| Dynamic Registration | ❌ None | ✅ Mount callbacks + registry |
| SPI Support | ✅ Full SPI mode | ⚠️ Adding now |
| SDMMC Support | ✅ Both modes | ✅ Existing |
| Bus Sharing | ✅ Proper SPI integration | ⚠️ Adding now |

## Code References

### Reference Repository Files
- `/tmp/reference_sd_card/components/sd_mmc_card/__init__.py` - Python config
- `/tmp/reference_sd_card/components/sd_mmc_card/sd_spi_card.h` - SPI header
- `/tmp/reference_sd_card/components/sd_mmc_card/sd_spi_card_esp_idf.cpp` - SPI implementation
- `/tmp/reference_sd_card/components/sd_mmc_card/sd_card.h` - Base interface

### Our Files to Modify
- `/workspaces/esphome/esphome/components/sd_storage/__init__.py`
- `/workspaces/esphome/esphome/components/sd_storage/sd_storage.h`
- `/workspaces/esphome/esphome/components/sd_storage/sd_storage.cpp`

### New Files to Create
- `/workspaces/esphome/esphome/components/sd_storage/sd_storage_spi.h`
- `/workspaces/esphome/esphome/components/sd_storage/sd_storage_spi.cpp`

## Notes
- SPI mode only works with ESP-IDF framework
- SPI mode requires 1-bit mode (no 4-bit SPI)
- ESP32-C6 doesn't have SDMMC hardware, requires SPI mode
- Pin mapping is different between modes
- Must use `sdspi_host_init()` and `esp_vfs_fat_sdspi_mount()` for SPI
- Must use `sdmmc_host_init_slot()` and `esp_vfs_fat_sdmmc_mount()` for SDMMC
