# esphome-esp32-platform

ESPHome `dev` plus the esp32 / psram platform changes of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome)
`testing_dev`, split into one branch per change. `main` merges every branch. Base:
ESPHome `dev` at `1f70f86b47` ([atm90e32] Add configurable thresholds (#15244)).

The code is taken over unchanged from `testing_dev`. Other topic repos (storage interface, camera) carry the
remaining parts of the same files: exFAT, FAT volume count, the storage-backed preferences, the hardware JPEG
selection.

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/esp32-warn-once` | config-time warnings once per run | all | - |
| `pr/esp32-idf-component-override-path` | `add_idf_component(override_path=...)`, a built-in IDF component replaced by a project copy | all | - |
| `pr/esp32-loop-task-core` | `LOOP_TASK_CORE` in `esp32/hal.h`, the loop task pinned to it | all (no affinity on single core) | - |
| `pr/esp32-nvs-ready` | `nvs_ready`: the default NVS partition formatted when it reports no free pages or a new version | all | - |
| `pr/esp32-nvs-defaults` | `add_nvs_default()`: defaults written into an NVS partition image at build time | all | - |
| `pr/esp32-chip-revision` | ESP32-P4 `minimum_chip_revision`, settings from the detected chip revision (`ESPHOME_CHIP_REVISION`), API action `chip_revision` | ESP32, P4 | `pr/esp32-warn-once` |
| `pr/esp32-flash-detect` | flash chip and speed from the detected flash (`ESPHOME_FLASH_ID`, `ESPHOME_FLASH_MAX_FREQUENCY`), API actions `flash_info` / `flash_verify`, `main_flash_80mhz_capable()` | all; speed: P4 | `pr/esp32-chip-revision` |
| `pr/esp32-coredump` | crash dumps to a `coredump` partition, API actions `coredump_info` / `coredump_read` / `coredump_erase` | all | `pr/esp32-chip-revision` |
| `pr/esp32-nvs-actions` | NVS browsing and erasing, API actions `nvs_list` / `nvs_erase` | all | `pr/esp32-chip-revision` |
| `pr/esp32-system-info` | `system_info.h`: chip, flash, PSRAM and partition facts, usage per region | all | - |
| `pr/esp32-p4-restart-fix` | `esp_restart_noos` wrapped: the other core stalled and the cache sync engine idle before the restart (espressif/esp-idf#19171) | P4 | - |
| `pr/esp32-p4-small-alloc-retention` | small malloc / new allocations to the retention RAM | P4 | - |
| `pr/esp32-boot-ram-probe` | `advanced: boot_ram_probe`: internal and internal DMA free RAM sampled for the first 90 s, logged afterwards | all | - |
| `pr/esp32-components-in-psram` | `advanced: components_in_psram`: the component objects in a PSRAM noinit section (needs `execute_from_psram`) | chips with PSRAM | - |
| `pr/esp32-board-s31-function-coreboard-1` | board `esp32-s31-function-coreboard-1` | S31 | - |
| `pr/esp32-idf61-hosted-sdkconfig` | IDF 6.1 Kconfig `default 0` patched; SoftAP / PHY TX power options skipped with `esp32_hosted` or IDF 6.1 | all | - |
| `pr/esp32-cert-bundle-sha512` | the certificate bundle keeps SHA-384 / SHA-512 | all | - |
| `pr/psram-p4-speeds` | ESP32-P4 PSRAM speeds 20 / 80 / 200 / 250 MHz, 250 MHz only on rev 3 | P4 | - |
| `pr/psram-ext-stack-guaranteed` | `FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` only when PSRAM is guaranteed | chips with PSRAM | - |

Each branch is ESPHome `dev` (the base above) plus that change; a branch in "Needs" is contained in it.

## Differences to upstream worth knowing

- `pr/esp32-idf61-hosted-sdkconfig` replaces upstream's `CORE.add_job(_reconcile_certificate_bundle_sdkconfig)`
  with the Kconfig patch job, as `testing_dev` has it.
- `pr/esp32-idf-component-override-path` carries `testing_dev`'s `add_idf_component()`, which also has the
  parameters `refresh`, `components` and `submodules` (deprecated there).

## Use

The whole topic:

```yaml
external_components:
  - source: github://p1ngb4ck/p1ngb4ck-esphome-esp32-platform@main
    components: [esp32, psram]
```

One change only:

```yaml
external_components:
  - source: github://p1ngb4ck/p1ngb4ck-esphome-esp32-platform@pr/esp32-coredump
    components: [esp32]
```

`external_components` replaces components only; the ESPHome core of the build host stays as installed.

Options:

```yaml
esp32:
  framework:
    type: esp-idf
    advanced:
      minimum_chip_revision: "3.0"
      boot_ram_probe: false
      components_in_psram: false
```

- `minimum_chip_revision` on the ESP32-P4: `0.0`, `0.1`, `1.0` (with `engineering_sample: true`), `3.0`, `3.1`.
  Without it, a build with `ESPHOME_CHIP_REVISION` set (revision x 100, e.g. `301`) takes the highest fitting value.
- A build with `ESPHOME_FLASH_ID` set (JEDEC ID, hex) takes `flash_chip` from the vendor and, on the P4,
  `flash_frequency` 80 MHz when the vendor is known and `minimum_chip_revision` is above 0.0, else 40 MHz (also
  40 MHz with `ESPHOME_FLASH_MAX_FREQUENCY=40MHZ`). A configured value always wins, with a warning.
- With `api:` in the config the API actions `chip_revision`, `flash_info`, `flash_verify`, `nvs_list`, `nvs_erase`
  and, with a `coredump` partition, `coredump_info`, `coredump_read`, `coredump_erase` are registered.
- A coredump partition in the partition table turns on crash dumps to flash.

## Combined branch

All topic repos together: `github://p1ngb4ck/esphome@topics-combined` (being built).
