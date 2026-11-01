# Configuration reference

New options of `esp32` and `psram`; everything else is as documented on
[esphome.io](https://esphome.io/components/esp32/). The first steps are in [README.md](README.md).

## `esp32`

```yaml
esp32:
  board: esp32-s31-function-coreboard-1
  framework:
    type: esp-idf
    advanced:
      minimum_chip_revision: "3.0"
      boot_ram_probe: false
      components_in_psram: false
```

| Option | Default | Description |
|---|---|---|
| `minimum_chip_revision` | from `ESPHOME_CHIP_REVISION`, else the variant's default | ESP32-P4: `0.0`, `0.1`, `1.0` (with `engineering_sample: true`), `3.0`, `3.1`. |
| `boot_ram_probe` | `false` | Sample free internal and internal DMA RAM for the first 90 s after boot and log the minimum. |
| `components_in_psram` | `false` | Place the component objects in a PSRAM section (needs `execute_from_psram`). |
| `board: esp32-s31-function-coreboard-1` | - | Espressif ESP32-S31 Function CoreBoard 1. |

### Build variables

A build service can pass what it detected on the device:

| Variable | Effect |
|---|---|
| `ESPHOME_CHIP_REVISION` | Revision x 100 (`301` = v3.1). Without `minimum_chip_revision` the highest fitting value is used. |
| `ESPHOME_FLASH_ID` | JEDEC ID (hex): `flash_chip` from the vendor; on the ESP32-P4 `flash_frequency` 80 MHz when the vendor is known and the revision is above 0.0, else 40 MHz. |
| `ESPHOME_FLASH_MAX_FREQUENCY` | `40MHZ` caps the flash at 40 MHz. |

A configured value always wins, with a warning.

### Crash dumps

A partition named `coredump` (type data, subtype coredump) in the partition table turns on crash dumps to flash.
With `api:` the actions `coredump_info`, `coredump_read` and `coredump_erase` read and clear them.

### API actions

With `api:` in the config: `chip_revision`, `flash_info`, `flash_verify`, `nvs_list`, `nvs_erase` (and the coredump
actions above).

## `psram`

| Option | Default | Description |
|---|---|---|
| `speed` | the variant's default | ESP32-P4: `20MHz`, `80MHz`, `200MHz`, `250MHz` (250 MHz only on chip revision 3, not with `engineering_sample`). |

Task stacks in PSRAM (`FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`) are allowed only when PSRAM is guaranteed (not
`ignore_not_found`).

## For component authors

| Helper | What |
|---|---|
| `esp32.add_idf_component(..., override_path=...)` | Replace a built-in IDF component by a project copy (patched FatFs, Bluetooth, USB). |
| `LOOP_TASK_CORE` (`esp32/hal.h`) | The main loop's core; pin tasks that work with the loop to it. |
| `nvs_ready` | Wait for the default NVS before using it (formatted when full or of a new version). |
| `esp32.add_nvs_default()` | Defaults written into an NVS partition image at build time. |
| `system_info.h` | Chip, flash, PSRAM and partition facts and usage per memory region. |
| `esp32.require_mbedtls_sha512()` | Keep SHA-384 / SHA-512 in mbedTLS and the certificate bundle. |
