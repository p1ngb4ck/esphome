# esphome-storage-interface

ESPHome `dev` plus the storage interface of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`:
the `storage` component, exFAT and the FAT volume sizing in `esp32`, the file upload path of `web_server_idf` and
the file API / file browser of `web_server`. One branch per change; `main` contains every branch. Base: ESPHome
`dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`.

Storage drivers (SD card, NFS, SPI flash, ...) are in
[esphome-storage-drivers](https://github.com/p1ngb4ck/esphome-storage-drivers), the USB ones in
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/esp32-exfat-fat-volumes` | `esp32` `advanced: enable_exfat` (patched FatFs copy), FAT volume count from the configured mounts, VFS max count, FatFs linked into `src` without exFAT | all ESP32, ESP-IDF | `pr/esp32-idf-component-override-path` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |
| `pr/storage` | the `storage` component: registry, `PathStorage` / `FilesystemStorage` / `NetworkStorage` / `RawStorage`, `StorageWorker` (async jobs, streams), `storage.*` actions, PSRAM transfer buffer, change feed, ISO 9660 mount | all ESP32 | `pr/esp32-exfat-fat-volumes`, `pr/esp32-loop-task-core` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |
| `pr/web-server-idf-file-uploads` | `web_server_idf`: multipart uploads for the file API, async uploads opt-in per handler, an explicit empty file created | ESP-IDF | - |
| `pr/web-server-file-api` | `web_server`: `file_api`, `raw_api`, `file_browser` (`simple`, or `advanced` with the vendored js-fileexplorer, MIT) | ESP-IDF, `version: 3` | `pr/storage`, `pr/web-server-idf-file-uploads` |

A branch contains the branches it needs (as merges).

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-storage-interface@main
    components: [esp32, storage, web_server, web_server_idf]

esp32:
  framework:
    type: esp-idf
    advanced:
      enable_exfat: true

storage:
  id: storage_registry
  psram_transfer_buffer_size: 2MB
  max_streams: 4

web_server:
  version: 3
  file_api:
    max_dir_entries: 64
  file_browser:
    variant: advanced
```

`storage:` alone mounts nothing; the drivers (`sd_storage`, `usb_storage`, `nfs_client`, `binary_storage`, ...)
register their storages with it. `external_components` replaces components only; the ESPHome core of the build host
stays as installed. The `esp32` here is the one of this topic only; with the other topics together use the combined
branch `github://p1ngb4ck/esphome@topics-combined`.

The component's own reference: `esphome/components/storage/.ai/storage-interface.md`.
