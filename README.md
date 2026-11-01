# esphome-storage-drivers

Storage drivers for the ESPHome storage interface
([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)): SD cards, NFS shares,
FTP / FTPS / TFTP servers (and an FTP / TFTP server on the device), S3 buckets, EEPROM / FRAM / MRAM / SPI flash
chips and flash partitions, preferences on a storage or an external NVS, firmware updates from a storage, and the
firmware's own YAML embedded for recovery. Every storage appears under its mount path (`/sdcard`, `/nfs`, `/ftp`,
...) and works with every consumer of the interface: file actions, the web server's file browser, players.

USB drives and optical drives are in [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

## Getting started

An SD card in 4-bit SD-MMC mode (ESP32-S3, ESP32-P4):

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-storage-drivers@main
    components: [storage, sd_storage]

storage:

sd_storage:
  clk_pin: GPIO43
  cmd_pin: GPIO44
  data0_pin: GPIO39
  data1_pin: GPIO40
  data2_pin: GPIO41
  data3_pin: GPIO42
```

An NFS share beside it:

```yaml
nfs_client:
  - server: 192.168.1.10
    export: /srv/share
    mount_path: /nfs
```

An SPI flash chip as a LittleFS file system:

```yaml
spi:
  clk_pin: GPIO18
  mosi_pin: GPIO23
  miso_pin: GPIO19

binary_storage:
  - type: spi_flash
    id: extra_flash
    cs_pin: GPIO5
    regions:
      - format: littlefs
        mount_path: /flash
```

Use your board's pins and your server's address.

## More

**[CONFIGURATION.md](CONFIGURATION.md)** has every option of every driver with an example per variant:

- `sd_storage`: SD-MMC (1 / 4 bit) and SPI, card detect, exFAT, actions
- `nfs_client`, `ftp` (FTP / FTPS / TFTP clients, FTP and TFTP server), `s3_client`
- `binary_storage`: I2C EEPROM and FRAM, SPI flash, FRAM and MRAM, 1-Wire EEPROM, flash partitions, NVS partitions;
  regions as raw, LittleFS, key-value or NVS; pre-filled files
- `preferences`: preferences on a storage or an external NVS, export and import
- `spi`: bus pins from the NVS
- `firmware_update`, `store_yaml`

TLS certificates for `ftp` and `s3_client` come from
[esphome-cert-store](https://github.com/p1ngb4ck/esphome-cert-store).

## Branches

One branch per component or change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/sd-storage` | `sd_storage`: SD card over SD-MMC (1 / 4 bit) or SPI | SPI: all ESP32; SD-MMC: S3, S31, P4 | `pr/storage` |
| `pr/nfs-client` | `nfs_client`: an NFS export as a network storage | all ESP32 | `pr/storage` |
| `pr/ftp` | `ftp`: FTP / FTPS / TFTP clients as network storages, an FTP server and a TFTP server on a storage | all ESP32 | `pr/storage`, `pr/cert-store` |
| `pr/s3-client` | `s3_client`: an S3 bucket as a network storage, TLS through `cert_store` | all ESP32 | `pr/storage`, `pr/cert-store` |
| `pr/firmware-update` | `firmware_update`: main firmware update from a manifest on a storage, non-blocking through the storage worker | all ESP32 | `pr/storage` |
| `pr/store-yaml` | `store_yaml`: the firmware's YAML embedded and exported to a storage | ESP32, host | `pr/storage` |
| `pr/spi-nvs-pins` | `spi`: bus pins, SPI flash CS and data lines 2 / 3 read from the internal NVS | all ESP32 | - |
| `pr/binary-storage` | `binary_storage`: EEPROM, FRAM, MRAM and SPI flash on I2C / SPI / 1-Wire as raw or file system storages, flash partitions, LittleFS regions, NVS regions, pre-fill | all ESP32 | `pr/storage`, `pr/spi-nvs-pins` |
| `pr/esp32-nvs-ready` | `nvs_ready`: the default NVS partition formatted when it reports no free pages or a new version | all ESP32 | - |
| `pr/esp32-nvs-defaults` | `add_nvs_default()`: defaults written into an NVS partition image at build time | all ESP32 | - |
| `pr/esp32-nvs-actions` | NVS browsing and erasing, API actions `nvs_list` / `nvs_erase` | all ESP32 | `pr/esp32-chip-revision` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |
| `pr/preferences-storage` | `preferences`: flash preferences through the storage interface or an external NVS region, optional internal NVS, `keep_early`, backup / restore | ESP32 | `pr/binary-storage`, `pr/esp32-nvs-ready` |
| `pr/nvs` | `nvs`: the NVS guard - what wrote the NVS (meta record), boot check, a policy per result, protected namespaces, actions, state sensor; `preferences: nvs_backup_dir`; the `nvs` text platform | ESP32 | `pr/preferences-storage`, `pr/esp32-nvs-actions` |

`pr/storage` is in [esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface),
`pr/cert-store` in [esphome-cert-store](https://github.com/p1ngb4ck/esphome-cert-store), `pr/esp32-nvs-ready` in
[esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform). `firmware_update` and
`binary_storage`'s pre-fill with partition access build on reworked `ota` partition handling that is not published
yet.

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).
- Third-party code keeps its own licence:
  - `store_yaml/zstddeclib.c`: Zstandard decoder (Meta Platforms), BSD.

## Thanks

Thank you, Mallow ([youkorr](https://github.com/youkorr)) - your SD card component for ESPHome was where
`sd_storage` started. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
