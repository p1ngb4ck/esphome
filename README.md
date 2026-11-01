# esphome-esp32-platform

Changes to ESPHome's `esp32` and `psram` platform components: settings from the detected chip revision and flash
chip (one firmware for different board revisions), crash dumps to flash with API access, NVS browsing and erasing
over the API, chip / flash / PSRAM facts for other components, ESP32-P4 PSRAM speeds up to 250 MHz, the ESP32-S31
Function CoreBoard, fixes for the ESP32-P4 restart and ESP-IDF 6.1, and helpers other topics build on (replacing an
IDF component, the loop task's core, NVS defaults at build time).

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-esp32-platform@main
    components: [esp32, psram]

esp32:
  variant: esp32p4
  framework:
    type: esp-idf
    advanced:
      minimum_chip_revision: "3.0"

psram:
  speed: 250MHz
```

With `api:` the device answers the API actions `chip_revision`, `flash_info`, `flash_verify`, `nvs_list`,
`nvs_erase` and, with a `coredump` partition in the partition table, `coredump_info`, `coredump_read`,
`coredump_erase`.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option, the build variables for chip revision and flash detection,
crash dumps, the helpers for component authors.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/esp32-warn-once` | config-time warnings once per run | all | - |
| `pr/esp32-idf-component-override-path` | `add_idf_component(override_path=...)`: a built-in IDF component replaced by a project copy | all | - |
| `pr/esp32-loop-task-core` | `LOOP_TASK_CORE` in `esp32/hal.h`, the loop task pinned to it | all | - |
| `pr/esp32-nvs-ready` | `nvs_ready`: the default NVS partition formatted when it reports no free pages or a new version | all | - |
| `pr/esp32-nvs-defaults` | `add_nvs_default()`: defaults written into an NVS partition image at build time | all | - |
| `pr/esp32-chip-revision` | ESP32-P4 `minimum_chip_revision`, settings from the detected chip revision, API action `chip_revision` | ESP32, P4 | `pr/esp32-warn-once` |
| `pr/esp32-flash-detect` | flash chip and speed from the detected flash, API actions `flash_info` / `flash_verify` | all; speed: P4 | `pr/esp32-chip-revision` |
| `pr/esp32-coredump` | crash dumps to a `coredump` partition, API actions `coredump_info` / `coredump_read` / `coredump_erase` | all | `pr/esp32-chip-revision` |
| `pr/esp32-nvs-actions` | NVS browsing and erasing, API actions `nvs_list` / `nvs_erase` | all | `pr/esp32-chip-revision` |
| `pr/esp32-system-info` | `system_info.h`: chip, flash, PSRAM and partition facts, usage per region | all | - |
| `pr/esp32-p4-restart-fix` | `esp_restart_noos` wrapped: the other core stalled and the cache sync engine idle before the restart (espressif/esp-idf#19171) | P4 | - |
| `pr/esp32-p4-small-alloc-retention` | small malloc / new allocations to the retention RAM | P4 | - |
| `pr/esp32-boot-ram-probe` | `advanced: boot_ram_probe`: free internal RAM sampled for the first 90 s, logged afterwards | all | - |
| `pr/esp32-components-in-psram` | `advanced: components_in_psram`: the component objects in a PSRAM section | chips with PSRAM | - |
| `pr/esp32-board-s31-function-coreboard-1` | board `esp32-s31-function-coreboard-1` | S31 | - |
| `pr/esp32-idf61-hosted-sdkconfig` | IDF 6.1 Kconfig `default 0` patched; SoftAP / PHY TX power options skipped with `esp32_hosted` or IDF 6.1 | all | - |
| `pr/esp32-cert-bundle-sha512` | the certificate bundle keeps SHA-384 / SHA-512 | all | - |
| `pr/psram-p4-speeds` | ESP32-P4 PSRAM speeds 20 / 80 / 200 / 250 MHz, 250 MHz only on revision 3 | P4 | - |
| `pr/psram-ext-stack-guaranteed` | task stacks in PSRAM allowed only when PSRAM is guaranteed | chips with PSRAM | - |

Single changes can be used on their own: `github://p1ngb4ck/esphome-esp32-platform@pr/esp32-coredump`. All topics
together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome core of the
build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
