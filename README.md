# esphome-storage-drivers

ESPHome `dev` plus the storage drivers of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev` on
the storage interface ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)): SD card,
NFS, SPI / I2C / 1-Wire NVM, firmware updates from a storage, the embedded YAML, preferences on a storage or an
external NVS. One branch per component or change; `main` contains every branch. Base: ESPHome `dev` at
`1f70f86b47`. The code is taken over unchanged from `testing_dev` (all files of these components identical).

None of them needs `cert_store`. The USB drivers (usb_storage, usb_cdrom) are in
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/sd-storage` | `sd_storage`: SD card over SD-MMC (1 / 4 bit) or SPI | SPI: all ESP32; SD-MMC: S3, S31, P4 | `pr/storage` |
| `pr/nfs-client` | `nfs_client`: an NFS export as a network storage | all ESP32 | `pr/storage` |
| `pr/firmware-update` | `firmware_update`: main firmware update from a manifest on a storage, non-blocking through the storage worker | all ESP32 | `pr/storage` |
| `pr/store-yaml` | `store_yaml`: the firmware's YAML embedded and exported to a storage | ESP32, host | `pr/storage` |
| `pr/spi-nvs-pins` | `spi`: bus pins, SPI flash CS and data lines 2 / 3 read from the internal NVS (`nvs_pins`, `nvs_cs_pin`) | all ESP32 | - |
| `pr/binary-storage` | `binary_storage`: EEPROM, FRAM, MRAM and SPI flash on I2C / SPI / 1-Wire as raw or filesystem storages, flash partitions, littlefs regions, an NVS region, prefill | all ESP32 | `pr/storage`, `pr/spi-nvs-pins` |
| `pr/preferences-storage` | `preferences` and `esp32` preferences: flash preferences through the storage interface or an external NVS region (`external_nvs`), optional internal NVS (`use_internal_nvs`), `keep_early`, backup / restore with every entry's NVS key and type | ESP32 | `pr/binary-storage`, `pr/esp32-nvs-ready` |

`pr/storage` is in [esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface),
`pr/esp32-nvs-ready` in [esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform); a branch
contains the branches it needs (as merges). `binary_storage`'s prefill with OTA partition access
(`USE_OTA_PARTITIONS`) uses the OTA data-partition listener of the OTA system topic.

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-storage-drivers@main
    components: [esp32, spi, storage, sd_storage, nfs_client, binary_storage, preferences, store_yaml]

storage:
  id: storage_registry

sd_storage:
  id: sd_card
  clk_pin: GPIO43
  cmd_pin: GPIO44
  data0_pin: GPIO39
  data1_pin: GPIO40
  data2_pin: GPIO41
  data3_pin: GPIO42
  mode_1bit: false
  data_rate: 40MHz
  slot: 0

nfs_client:
  server: nfs-server
  uid: 1000
  gid: 1000
  export: "/opt/nfs/share"
  mount_path: "/nfs"

spi:
  - id: spi_bus_id
    type: quad
    clk_pin: GPIO06
    data_pins: [GPIO20, GPIO21, GPIO22, GPIO05]
    interface: spi2

binary_storage:
  - type: spi_flash
    spi_id: spi_bus_id
    id: extra_flash
    model: "MX25L256"
    capacity: 33554432B
    cs_pin: GPIO23
    data_rate: 40MHz
    flash_chip: mxic
    assume_exclusive_bus: true
    regions:
      - format: nvs
        size: 262144B
        label: nvs_store
        id: custom_nvs
      - format: littlefs
        label: "External flash"
        mount_path: /flash
        auto_format: true

preferences:
  external_nvs: custom_nvs
  use_internal_nvs: false

store_yaml:
```

The pins are those of the Waveshare ESP32-P4 Pi board with a 32 MB SPI flash on SPI2. `external_components`
replaces components only; the ESPHome core of the build host stays as installed. With the other topics together use
the combined branch `github://p1ngb4ck/esphome@topics-combined`.
