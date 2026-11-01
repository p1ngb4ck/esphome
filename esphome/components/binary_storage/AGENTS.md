# binary_storage: guide for AI agents

Non-volatile memory chips and flash areas as storages (read `esphome/components/storage/AGENTS.md` first). The
user documentation is `CONFIGURATION.md` of esphome-storage-drivers.

Classes:
- `BinaryStorage` (`RawStorage`): the chip drivers - `I2CEeprom`, `I2CFram`, `SPIFlash`, `SPIFram`, `SPIMRAM`,
  `OneWireEEPROM`. Byte read / write / erase with the chip's page and erase geometry (`RawGeometry`).
- Regions on a chip: `raw` (a `RawStorage` window), `littlefs` (`LittleFSMount`, a `FilesystemStorage` on a
  `FlashPartition` view), `kv` (`InplaceKVStore`), `nvs` (`NVSStore`, an ESP-IDF NVS on an `esp_partition`
  registered for the region - used by `preferences: external_nvs`).
- `flash_partition` / `nvs` types: areas of the chip's own flash.

Rules:
- Model tables in `__init__.py` give capacity, page size and addressing; explicit options override them. Model data
  stays there, not in C++.
- `IO_TASK_SAFE` is only reported with `assume_exclusive_bus` (validated: the chip alone on its bus); otherwise the
  storage worker slices the chip's I/O on the main loop.
- SPI flash on an exclusive ESP32 bus goes through ESP-IDF's `esp_flash` (vendor driver `flash_chip`, quad mode from
  the bus type) and `esp_partition`, so LittleFS and NVS use the IDF layers.
- `nvs_cs_pin` and `spi: nvs_pins` read the wiring from the internal NVS at boot (one firmware for different boards).
- `pre_fill` builds a LittleFS image at compile time; `_validate_prefill_fits` estimates the fit, the image build
  enforces it.
