# esphome-storage-interface

One storage interface for ESPHome: SD cards, USB drives, network shares, flash chips and more register with one
registry, are reached by one path scheme (`/sdcard/...`, `/usb/...`, `/nfs/...`) and share one asynchronous worker
for copies, moves, listings and file streams. On top of it: file actions for automations, exFAT, ISO images, and a
file API and file browser in the web server.

This repository holds the interface itself. The storage drivers are in
[esphome-storage-drivers](https://github.com/p1ngb4ck/esphome-storage-drivers) (SD card, NFS, FTP, S3, EEPROM /
FRAM / flash, ...) and [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers) (USB drives, optical
drives).

## Getting started

`storage:` alone mounts nothing; a driver registers its storage with it. With an SD card from
esphome-storage-drivers and the web server's file browser:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-storage-interface@main
    components: [esp32, storage, web_server, web_server_idf]
  - source: github://p1ngb4ck/esphome-storage-drivers@main
    components: [sd_storage]

storage:

sd_storage:
  clk_pin: GPIO43
  cmd_pin: GPIO44
  data0_pin: GPIO39
  data1_pin: GPIO40
  data2_pin: GPIO41
  data3_pin: GPIO42

web_server:
  version: 3
  file_api:
    max_dir_entries: 64
  file_browser:
    variant: advanced
```

Use your board's pins. The file browser is then at `http://<device>/files/`. Write a file from an automation:

```yaml
button:
  - platform: template
    name: Write log line
    on_press:
      - storage.file_append:
          path: /sdcard/log.txt
          content: "pressed"
```

## More

- **[CONFIGURATION.md](CONFIGURATION.md)** - every option of `storage`, all `storage.*` actions with examples, exFAT,
  ISO images, the web server's file API, raw API and file browser.
- Every storage driver documents its own options in its repository's `CONFIGURATION.md`.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/esp32-exfat-fat-volumes` | `esp32` `advanced: enable_exfat` (patched FatFs copy), FAT volume count from the configured mounts, VFS max count | all ESP32, ESP-IDF | `pr/esp32-idf-component-override-path` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |
| `pr/storage` | the `storage` component: registry, path / filesystem / network / raw storages, the asynchronous worker (jobs, streams), `storage.*` actions, PSRAM transfer buffer, change feed, ISO 9660 mount | all ESP32 | `pr/esp32-exfat-fat-volumes`, `pr/esp32-loop-task-core` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |
| `pr/storage-write-done` | `write_done()`: a storage is told when a written file is complete (network storages finish uploads there) | all ESP32 | `pr/storage` |
| `pr/web-server-idf-file-uploads` | `web_server_idf`: multipart uploads for the file API, async uploads per handler, an explicit empty file created | ESP-IDF | - |
| `pr/web-server-file-api` | `web_server`: `file_api`, `raw_api`, `file_browser` (`simple`, or `advanced` with js-fileexplorer) | ESP-IDF, `version: 3` | `pr/storage`, `pr/web-server-idf-file-uploads` |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

The advanced file browser is CubicleSoft's [js-fileexplorer](https://github.com/cubiclesoft/js-fileexplorer) (MIT) -
thank you for such a complete widget. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
