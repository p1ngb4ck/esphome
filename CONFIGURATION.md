# Configuration reference

Every option of the drivers in this repository, with an example per variant. The first steps are in
[README.md](README.md); the `storage:` block and the `storage.*` actions are documented in
[esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface/blob/main/CONFIGURATION.md).

- [`sd_storage`](#sd_storage) - SD cards
- [`nfs_client`](#nfs_client) - NFS shares
- [`ftp`](#ftp) - FTP / FTPS / TFTP clients, FTP and TFTP server
- [`s3_client`](#s3_client) - S3 buckets
- [`binary_storage`](#binary_storage) - EEPROM, FRAM, MRAM, SPI flash, flash partitions, NVS
- [`preferences`](#preferences) - preferences on a storage or an external NVS
- [`spi`: pins from the NVS](#spi-pins-from-the-nvs)
- [`firmware_update`](#firmware_update)
- [`store_yaml`](#store_yaml)

## `sd_storage`

An SD card as a FAT (or exFAT) file system storage. `type: sd_mmc` (default) uses the SD-MMC host (ESP32-S3,
ESP32-S31, ESP32-P4), `type: sd_spi` an SPI bus (every ESP32, ESP-IDF).

### SD-MMC

| Option | Default | Description |
|---|---|---|
| `clk_pin` | required | Clock pin. |
| `cmd_pin` | required | Command pin. |
| `data0_pin` | required | Data line 0. |
| `data1_pin`, `data2_pin`, `data3_pin` | - | Data lines 1-3 for 4-bit mode. |
| `mode_1bit` | `false` | Use data line 0 only. |
| `slot` | `0` | SD-MMC slot, 0 or 1. |
| `data_rate` | 40 MHz on the ESP32-P4, else 20 MHz | Bus clock, 400 kHz - 40 MHz. |
| `mount_path` | `/sdcard` | Mount point. |
| `cd_pin` | - | Card detect pin: insert and remove are handled at runtime. |
| `format_on_mismatch` | `false` | Format a card whose file system does not match `file_system`. |
| `file_system` | `auto` | Only with `esp32: ... advanced: enable_exfat: true`: `auto`, `fat32` or `exfat`. |
| `on_mounted`, `on_inserted`, `on_removed` | - | Automations for the card's state. |

```yaml
sd_storage:
  id: sd_card
  clk_pin: GPIO43
  cmd_pin: GPIO44
  data0_pin: GPIO39
  data1_pin: GPIO40
  data2_pin: GPIO41
  data3_pin: GPIO42
  cd_pin: GPIO45
  on_inserted:
    - logger.log: card inserted
```

### SPI

The card's pins are the SPI bus pins (`clk_pin`, `mosi_pin`, `miso_pin` of `spi:`); only 1-bit mode.

| Option | Default | Description |
|---|---|---|
| `cs_pin` | - | Chip select (from `spi_device`). |
| `spi_id` | the only bus | The SPI bus, needed with more than one. It must be a hardware SPI bus. |
| `data_rate` | the bus default | SPI clock. |
| `mount_path`, `cd_pin`, `format_on_mismatch`, `file_system`, `on_*` | | as above |
| `assume_exclusive_bus` | `false` | The card is the only device on its bus: its I/O may run on the storage worker's task. Refused when another device shares the bus. |

```yaml
spi:
  clk_pin: GPIO18
  mosi_pin: GPIO23
  miso_pin: GPIO19

sd_storage:
  type: sd_spi
  cs_pin: GPIO5
  assume_exclusive_bus: true
```

### Actions and condition

```yaml
- sd_storage.mount: sd_card
- sd_storage.unmount: sd_card
- sd_storage.list_files:
    id: sd_card
    path: /music
- if:
    condition:
      sd_storage.is_mounted: sd_card
    then:
      - logger.log: card ready
```

## `nfs_client`

NFS v3 exports as network storages. A list; every entry is one share.

| Option | Default | Description |
|---|---|---|
| `server` | required | Host name or address of the NFS server. |
| `export` | required | Exported path on the server. |
| `mount_path` | required | Mount point on the device. |
| `port` | `2049` | NFS port. |
| `uid` / `gid` | `0` | User and group ID the device presents (0-65535). |
| `auto_connect` | `true` | Connect when the network is up; `false` = only on `storage.mount`. |

```yaml
nfs_client:
  - id: media_share
    server: nas.local
    export: /srv/media
    mount_path: /nfs
    uid: 1000
    gid: 1000
```

## `ftp`

FTP, FTPS (explicit TLS) and TFTP servers as network storages, and an FTP server (with an optional TFTP server) that
serves a local storage.

### Clients

| Option | Default | Description |
|---|---|---|
| `protocol` | `ftp` | `ftp` or `tftp` (TFTP: ESP32 only, no login, no TLS). |
| `server` | required | Host name or address. |
| `mount_path` | required | Mount point on the device. |
| `port` | 21 (FTP), 69 (TFTP) | Server port. |
| `username` / `password` | required for FTP | Login. |
| `auto_connect` | `true` | Connect when the network is up. |
| `auth_tls` | `false` | FTPS: `AUTH TLS` on the control and data connections (ESP-IDF). |
| `ca`, `client_certificate`, `client_certificate_key` | - | TLS material, see [esphome-cert-store](https://github.com/p1ngb4ck/esphome-cert-store/blob/main/CONFIGURATION.md). Without `ca` the built-in CA bundle is used. |

```yaml
ftp:
  clients:
    - id: nas_ftp
      server: nas.local
      username: !secret ftp_user
      password: !secret ftp_password
      mount_path: /ftp
      auth_tls: true
    - protocol: tftp
      server: 192.168.1.20
      mount_path: /tftp
```

### Server

ESP32 only. Serves a storage folder over FTP; files go through the storage worker.

| Option | Default | Description |
|---|---|---|
| `root` | required | The served folder, a storage path such as `/sdcard`. |
| `users` | required | 1-8 users: `username`, `password`, `access` (`read_only` (default) or `read_write`). |
| `port` | `21` | Control port. |
| `max_sessions` | `2` | Simultaneous sessions, 1-4. |
| `tftp` | - | A TFTP server on the same root: `port` (`69`), `access` (`read_only`), `max_file_size` (`1MB`, 512 B - 32 MB). |

```yaml
ftp:
  server:
    root: /sdcard
    users:
      - username: admin
        password: !secret ftp_admin_password
        access: read_write
      - username: guest
        password: !secret ftp_guest_password
    tftp:
      access: read_only
```

## `s3_client`

An S3 bucket (AWS, MinIO, Garage, ...) as a network storage. A list; every entry is one bucket.

| Option | Default | Description |
|---|---|---|
| `endpoint` | required | Host name of the S3 endpoint. |
| `bucket` | required | Bucket name. |
| `access_key` / `secret_key` | required | Credentials. |
| `mount_path` | required | Mount point on the device. |
| `region` | `us-east-1` | Signing region. |
| `port` | 443 with TLS, else 80 | Endpoint port. |
| `tls` | `true` | HTTPS. |
| `path_style` | `true` | `endpoint/bucket/key` addressing; `false` = virtual-host style `bucket.endpoint/key`. |
| `base_path` | `/` | A prefix inside the bucket that is the storage's root. |
| `auto_connect` | `true` | Connect when the network is up. |
| `ca`, `client_certificate`, `client_certificate_key` | - | TLS material from cert_store; without `ca` the built-in CA bundle. |

```yaml
s3_client:
  - endpoint: minio.local
    port: 9000
    tls: false
    bucket: panel
    access_key: !secret s3_access_key
    secret_key: !secret s3_secret_key
    mount_path: /s3
```

## `binary_storage`

Non-volatile memory chips and flash areas as storages. A list; `type` selects the device. A chip is one storage by
default (raw byte access); `regions` split it into parts, each `raw`, `littlefs` (a file system), `kv` (key-value)
or `nvs` (an ESP-IDF NVS, e.g. for preferences).

| Type (aliases) | Bus | Default model |
|---|---|---|
| `i2c_eeprom` (`eeprom`) | I2C | `AT24C256` |
| `i2c_fram` (`fram`) | I2C | `MB85RC256` |
| `spi_flash` (`flash`) | SPI (single, dual, quad) | `W25Q32` |
| `spi_fram` | SPI | `FM25V10` |
| `spi_mram` (`mram`) | SPI | `MR25H256` |
| `onewire_eeprom` (`onewire`) | 1-Wire | `DS2431` |
| `flash_partition` (`partition`) | the chip's own flash | - |
| `nvs` | the chip's own flash | - |

### Common options of the chips

| Option | Default | Description |
|---|---|---|
| `id` | - | ID, used by actions and `preferences`. |
| `model` | per type | Known models set capacity, page size and addressing. |
| `capacity` | from the model | Size in bytes (`32kB`, `16MB`). |
| `addressing_bits` | from the model | I2C EEPROM 8/9/10/11/16, I2C FRAM 9/11/16/32, SPI FRAM / MRAM 16/24. |
| `page_size` | from the model | Write page: I2C EEPROM 8-128, 1-Wire 8-32, SPI flash 256. |
| `regions` | one raw region | Parts of the chip, see below. |
| `storage_name` | the type | Name shown in file browsers. |
| `device_node` | `true` with a file browser | Offer the raw side as a device node in file browsers. |
| `device_node_name` | the type | Name of that node. |
| `assume_exclusive_bus` | - | The chip is alone on its bus: its I/O may run on the storage worker's task. |

I2C chips also take `address` (default `0x50`) and `i2c_id`; SPI chips `cs_pin` (required), `spi_id`, `data_rate`,
`spi_mode`.

### SPI flash

| Option | Default | Description |
|---|---|---|
| `erase_size` | from the model | Erase unit: 4096, 32768 or 65536. |
| `jedec_id` | - | Expected JEDEC ID; a different chip is reported. |
| `io_mode` | from the bus | `single`, `dual` or `quad`. A flash on a `type: quad` bus runs in quad mode. |
| `flash_chip` | `generic` | Vendor driver of ESP-IDF's flash layer: `issi`, `gd`, `mxic`, `winbond`, `boya`, `th`. |
| `nvs_cs_pin` | - | Read CS and data lines 2 / 3 from the internal NVS (one firmware for boards with different wiring): `namespace` (required), keys `key` (`spi_flash_cs`), `lines` (`spi_lines`), `data2_pin` (`spi_d2`), `data3_pin` (`spi_d3`). |

### 1-Wire EEPROM

| Option | Default | Description |
|---|---|---|
| `pin` | required | The 1-Wire data pin. |
| `address` | the first device | 64-bit ROM address of the chip. |

### Regions

`size` is a byte size or `remaining` (default; at most one region per chip takes the rest). Regions are laid out in
order.

| Format | Options |
|---|---|
| `raw` | `size`, `label`, `storage_name` |
| `littlefs` | `size`, `mount_path` (default `/<chip id>`), `auto_format` (`true`), `label`, `storage_name` |
| `kv` | `size`, `storage_name` |
| `nvs` | `size`, `label`, `namespace` (`binary_storage`), `storage_name` |

### Flash partition (`flash_partition`)

A LittleFS file system in a partition of the chip's own flash.

| Option | Default | Description |
|---|---|---|
| `partition_label` | required | Partition label in the partition table. |
| `partition_size` | `512kB` | Partition size, at least 4 KB. |
| `mount_path` | `/littlefs` | Mount point. |
| `auto_format` | `true` | Format when the partition holds no file system. |
| `storage_name` | - | Name shown in file browsers. |
| `pre_fill` | - | Files built into the partition image: a list of `source` (a file next to the YAML) and `target` (absolute path in the file system). |

### NVS partition (`nvs`)

| Option | Default | Description |
|---|---|---|
| `partition_label` | required | Partition label. |
| `partition_size` | `64kB` | At least 12 KB. |
| `namespace` | `binary_storage` | NVS namespace, up to 15 characters. |
| `storage_name` | - | Name shown in file browsers. |

### Examples

I2C FRAM, raw:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22

binary_storage:
  - type: i2c_fram
    id: state_fram
    model: MB85RC256
    address: 0x50
```

I2C EEPROM as a small file system:

```yaml
binary_storage:
  - type: i2c_eeprom
    id: config_eeprom
    model: AT24C256
    address: 0x51
    regions:
      - format: littlefs
        mount_path: /eeprom
```

Quad SPI flash split into an NVS (for preferences) and a file system:

```yaml
spi:
  - id: flash_bus
    type: quad
    clk_pin: GPIO06
    data_pins: [GPIO20, GPIO21, GPIO22, GPIO05]
    interface: spi2

binary_storage:
  - type: spi_flash
    spi_id: flash_bus
    id: extra_flash
    model: MX25L256
    capacity: 32MB
    cs_pin: GPIO23
    data_rate: 40MHz
    flash_chip: mxic
    assume_exclusive_bus: true
    regions:
      - format: nvs
        size: 256kB
        label: nvs_store
        id: custom_nvs
      - format: littlefs
        mount_path: /flash
```

SPI FRAM and MRAM:

```yaml
binary_storage:
  - type: spi_fram
    id: log_fram
    model: FM25V10
    cs_pin: GPIO15
  - type: spi_mram
    id: state_mram
    model: MR25H256
    cs_pin: GPIO16
```

1-Wire EEPROM:

```yaml
binary_storage:
  - type: onewire_eeprom
    id: id_eeprom
    model: DS2431
    pin: GPIO4
```

A partition of the chip's flash with files built in:

```yaml
binary_storage:
  - type: flash_partition
    partition_label: assets
    partition_size: 1MB
    mount_path: /assets
    pre_fill:
      - source: www/index.html
        target: /index.html
```

An NVS partition:

```yaml
binary_storage:
  - type: nvs
    id: settings_nvs
    partition_label: settings
    partition_size: 64kB
```

### Actions

```yaml
- binary_storage.write:
    id: state_fram
    address: 0x0000
    data: [0x01, 0x02, 0x03, 0x04]
- binary_storage.write_byte:
    id: state_fram
    address: 0x0100
    value: 0xFF
- binary_storage.write_string:
    id: state_fram
    address: 0x0200
    value: "hello"
- binary_storage.fill:
    id: state_fram
    value: 0x00
- binary_storage.read:
    id: state_fram
    address: 0x0000
    length: 4
```

Bulk transfers (to and from files) are the `storage.raw_*` actions of the storage interface.

## `preferences`

ESP32 only. Where flash preferences live, and their backup.

| Option | Default | Description |
|---|---|---|
| `external_nvs` | - | The ID of an `nvs` region of a `binary_storage` chip: flash preferences live there. |
| `storage_backend` | `false` | Flash preferences through the storage interface. |
| `use_internal_nvs` | `true` | `false`: the internal NVS is no preferences backend (it is still initialised for WiFi / Bluetooth). |
| `keep_early` | - | Components that read their preferences before storages are up; their preferences stay in the early (RTC) store. |

With `external_nvs`, `safe_mode`'s boot counter stays in the RTC store, so a failing external chip cannot defeat it.
`use_internal_nvs: false` needs `external_nvs`; preferences read before the external store is up then return
their defaults unless their component is listed in `keep_early`.

```yaml
preferences:
  external_nvs: custom_nvs
  use_internal_nvs: false
```

### `preferences.export` / `preferences.import`

Back up and restore preferences, every entry with its NVS key and type.

| Option | Default | Description |
|---|---|---|
| `path` | - | A file: `format` `kv` (key=value lines) or `json` (needs `json:`). |
| `device` + `address` | - | A raw storage and an address: the entries as stored. |
| `preferences` | all | Only the preferences of these components. |
| `reboot` | `false` | Import only: restart afterwards. |

Exactly one of `path` and `device`.

```yaml
- preferences.export:
    path: /sdcard/backup/preferences.json
    format: json
- preferences.import:
    path: /sdcard/backup/preferences.json
    format: json
    reboot: true
```

## `spi`: pins from the NVS

ESP32 only. A bus's pins are read from the internal NVS at boot (one firmware for boards with different wiring).

| Option | Default | Description |
|---|---|---|
| `nvs_pins.namespace` | required | NVS namespace of the keys. |
| `nvs_pins.clk_pin` / `mosi_pin` / `miso_pin` | `spi_clk` / `spi_mosi` / `spi_miso` | Keys of the bus pins. |
| `nvs_pins.data2_pin` / `data3_pin` | `spi_d2` / `spi_d3` | Keys of data lines 2 and 3 (quad). |
| `nvs_pins.lines` | `spi_lines` | Key of the number of data lines. |

```yaml
spi:
  - id: flash_bus
    clk_pin: GPIO06
    mosi_pin: GPIO20
    miso_pin: GPIO21
    nvs_pins:
      namespace: board
```

## `firmware_update`

Updates the main firmware from a file on a storage, non-blocking (the image is read through the storage worker), with
an optional JSON manifest (version, name, summary, MD5).

| Option | Default | Description |
|---|---|---|
| `path` | - | Default image path (`/sdcard/firmware.bin`). |
| `allow_partition_access` | `false` | ESP32: images that also carry partition table and data partitions. |

Plus the options of ESPHome's OTA platforms (`on_begin`, `on_progress`, `on_end`, `on_error`, ...).

```yaml
firmware_update:
  id: sd_update
  path: /sdcard/update/firmware.bin

button:
  - platform: template
    name: Update from SD
    on_press:
      - firmware_update.flash: sd_update
```

## `store_yaml`

Embeds the compressed YAML in the firmware so a device can give its configuration back.

| Option | Default | Description |
|---|---|---|
| `include_secrets` | `false` | Embed `!secret` values; otherwise they are replaced by a fill-in skeleton. |
| `allow_unencrypted` | `false` | Allow `store_yaml` with an unencrypted API (anyone on the network could read the YAML). |

```yaml
store_yaml:

button:
  - platform: template
    name: Export YAML
    on_press:
      - store_yaml.export_to_storage:
          path: /sdcard/config.yaml
```

`raw: true` on the action writes the compressed blob as it is.
