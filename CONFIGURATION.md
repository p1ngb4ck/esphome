# Configuration reference

Every option of the components and changes in this repository. The first steps are in [README.md](README.md).

- [`storage`](#storage)
- [Actions and conditions](#actions-and-conditions)
- [ISO images](#iso-images)
- [`esp32`: exFAT](#esp32-exfat)
- [`web_server`: file API, raw API, file browser](#web_server-file-api-raw-api-file-browser)

## `storage`

The registry every storage driver registers with, and the asynchronous worker the drivers and consumers share. All
paths are full paths below a storage's mount point (`/sdcard/music/a.mp3`).

| Option | Default | Description |
|---|---|---|
| `id` | - | ID of the registry. |
| `enable_psram_transfer_buffer` | `true` with `psram:` | A shared PSRAM buffer that copies, uploads and streams borrow for one operation at a time. Only with the `psram` component. |
| `psram_transfer_buffer_size` | 25 % of the PSRAM | Size of the transfer buffer, at least 64 KB. A size above 80 % of the detected PSRAM disables the buffer. |
| `psram_transfer_buffer_override_limit` | `false` | Allow a transfer buffer above 80 % of the PSRAM. |
| `copy_chunk_size` | `16384` | Chunk size of copies without the transfer buffer, 4096-131072, a multiple of 512. |
| `path_max` | the longest a driver needs | Longest path relative to a storage, 64-1024 bytes. Larger values use more stack in tree walks. |
| `max_blocking_transfer_size` | `16384` | Largest file the blocking helpers (`storage.file_read`, preferences import / export) hold in RAM; `0` = no limit. Larger files go through the worker. |
| `move_fallback_copy` | `true` | A move the storage refuses as a rename (e.g. across file systems on an NFS export) is done as copy and delete. `false` reports the refusal. |
| `task_stack_size` | `8192` | Stack of the worker task, 4096-32768 bytes. |
| `task_priority` | `9` | FreeRTOS priority of the worker task, 1-23. |
| `max_pending` | `4` | Jobs (copies, moves, listings, mounts, ...) queued at once, 1-16. A further job is refused with "not ready". |
| `max_streams` | `2` | File streams (uploads, downloads, players reading a file) open at once, 1-8. |
| `worker_update_interval` | `5ms` | How often the worker moves its jobs on when no task does the work. |
| `on_registered` | - | Automation when a storage registers (inserted card, plugged drive, connected share). `x` is the storage. |
| `on_unregistered` | - | Automation when a storage goes away. `x` is the storage. |
| `iso_mount` | - | ISO images as read-only storages, see [ISO images](#iso-images). |

The worker task, its queue and its streams exist only when a driver needs them; without one, the `task_*`,
`max_pending` and `max_streams` options have no effect.

```yaml
storage:
  id: storage_registry
  psram_transfer_buffer_size: 2MB
  max_streams: 4
  on_registered:
    - logger.log: storage registered
```

## Actions and conditions

All actions take full paths. `on_complete` receives the error text as `x` (empty on success).

### `storage.file_write` / `storage.file_append`

Write a text to a file (replaced) or append it.

| Option | Default | Description |
|---|---|---|
| `path` | required | File path, templatable. |
| `content` | - | The text, templatable. |
| `format` / `args` | - | A printf format and its arguments instead of `content`. |
| `newline` | `false` (write), `true` (append) | Add a line end. |
| `on_complete` | - | After the write. |

```yaml
- storage.file_append:
    path: /sdcard/temperature.csv
    format: "%u,%.1f"
    args: [millis(), id(temp).state]
```

### `storage.file_read`

Read a small file (up to `max_blocking_transfer_size`) and extract a value.

| Option | Default | Description |
|---|---|---|
| `path` | required | File path, templatable. |
| `extract` | `[]` | Steps applied in order, each exactly one of: `line: N` (the Nth line), `split: <sep>` with `index: N`, `json: a/b/0` (needs `json:`), `key: <name>` with `separator: <sep>` (a `key<sep>value` line), `regex: <re>` with `group: N`, `trim: true`. |
| `to_global` | - | A `globals` ID that receives the value. |
| `on_value` | - | Automation with the value as `x`. |
| `on_error` | - | Automation with the reason as `x` (unreadable file, a step that did not match). |

One of `to_global` and `on_value` is required.

```yaml
- storage.file_read:
    path: /sdcard/config.txt
    extract:
      - key: volume
        separator: "="
      - trim: true
    on_value:
      - logger.log:
          format: "volume %s"
          args: [x.c_str()]
```

### `storage.file_copy` / `storage.file_move`

Copy or move a file or a whole folder on the worker, also between storages. A move on one storage is a rename.

| Option | Default | Description |
|---|---|---|
| `from` | required | Source path, templatable. |
| `to` | required | Target path, templatable. |
| `on_complete` | - | When the job is done. |

### `storage.file_delete`

| Option | Default | Description |
|---|---|---|
| `path` | required | File or folder, templatable. |
| `recursive` | `false` | Delete a folder with its contents. |
| `on_complete` | - | After the delete. |

### `storage.file_exists` (condition)

```yaml
- if:
    condition:
      storage.file_exists:
        path: /sdcard/update.bin
    then:
      - logger.log: update found
```

### `storage.stat`

| Option | Default | Description |
|---|---|---|
| `path` | required | File or folder, templatable. |
| `on_exists` | - | The path exists. |
| `on_missing` | - | It does not. |
| `on_error` | - | The storage could not answer; `x` is the reason. |

### `storage.list_dir`

| Option | Default | Description |
|---|---|---|
| `path` | required | Folder, templatable. |
| `max_entries` | `256` | Entries delivered at most. |
| `on_entry` | - | Per entry: `name`, `size`, `is_dir`, `mtime` (0 = unknown). |
| `on_complete` | - | After the last entry. |
| `on_error` | - | `x` is the reason. |

```yaml
- storage.list_dir:
    path: /sdcard/music
    on_entry:
      - logger.log:
          format: "%s %llu"
          args: [name.c_str(), size]
```

### `storage.mount` / `storage.unmount` / `storage.format`

`id` (required) is the storage; `on_complete` as above. `storage.unmount` also ejects a USB drive safely.
`storage.format` erases the storage.

### `storage.raw_read` / `storage.raw_write` / `storage.raw_erase`

Byte access to raw storages (flash chips, EEPROM, FRAM, ...).

| Action | Options |
|---|---|
| `storage.raw_read` | `id` (required), `address`, `size` (`0` = to the end with `to_file`), `to_file` (a file path: the read lands there, through the worker) or `on_value` (`x`: the bytes), `on_complete` |
| `storage.raw_write` | `id` (required), `address`, `data` (bytes) or `from_file` (a file path), `erase_first` (`false`; erase the range before writing), `on_complete` |
| `storage.raw_erase` | `id` (required), `address` and `size`, or `all: true`; `force_sliced_erase` (`false`; erase sector by sector even where the chip could erase it at once), `on_complete` |

```yaml
- storage.raw_read:
    id: extra_flash
    address: 0x0
    size: 0
    to_file: /sdcard/flash-backup.bin
```

## ISO images

```yaml
storage:
  iso_mount:
    mount_path: /iso
    max_images: 1
```

| Option | Default | Description |
|---|---|---|
| `mount_path` | `/iso` | Mount point of the first image; further images get `/iso2`, `/iso3`, ... Up to 29 characters. |
| `max_images` | `1` | Images mounted at once, 1-9. |

```yaml
- storage.iso_mount:
    file: /sdcard/images/disc.iso
- storage.iso_unmount: /iso
```

Both actions take `on_complete`. An image is read through the worker, read-only.

## `esp32`: exFAT

```yaml
esp32:
  framework:
    type: esp-idf
    advanced:
      enable_exfat: true
```

| Option | Default | Description |
|---|---|---|
| `enable_exfat` | `false` | exFAT in FatFs (a patched copy of ESP-IDF's FatFs). |

With exFAT on, drivers that mount FAT media (`sd_storage`, `usb_storage`) get the option `file_system`: `auto`
(default, the medium's own), `fat32` or `exfat` (an explicit choice formats a medium with the other file system,
together with the driver's `format_on_mismatch`). Without `enable_exfat` the option does not exist and every medium
is FAT32. The number of FAT volumes follows the configured mounts.

## `web_server`: file API, raw API, file browser

Needs `web_server: version: 3` on ESP-IDF.

### `file_api`

HTTP endpoints under `/files/` for every path storage: `storages`, `list`, `stat`, `download`, `upload`, `mkdir`,
`copy`, `move`, `delete`, `mount`, `unmount`, `format`, `job` (progress of a copy / move), `changes` (folders changed
since a sequence number).

| Option | Default | Description |
|---|---|---|
| `storage_id` | all storages | Limit the API to one storage. |
| `max_dir_entries` | `64` | Entries in one `list` response, 1-1024. |
| `enable_list` | `true` | Listings. |
| `enable_read` | `true` | Downloads and `stat`. |
| `enable_write` | `true` | Uploads, folders, copies, moves. |
| `enable_delete` | `true` | Deletes. |
| `enable_mount` | `true` | Mounting (also ISO images). |
| `enable_unmount` | `true` | Unmounting / ejecting. |
| `enable_format` | `false` | Formatting. |

### `raw_api`

Byte access over HTTP to a raw storage.

| Option | Default | Description |
|---|---|---|
| `device_id` | - | The raw storage. |
| `enable_write` | `true` | Writes and erases; `false` = read-only. |

### `file_browser`

A file manager page at `/files/`, on top of `file_api`.

| Option | Default | Description |
|---|---|---|
| `variant` | `simple` | `simple` (built in, small) or `advanced` (js-fileexplorer: drag and drop, multi-select, text editor, live view of log files). |
| `asset_source` | `flash` | `advanced` only: the widget's files from the firmware (`flash`) or from a storage (`storage`, smaller firmware). |
| `asset_path` | `/sdcard/file-explorer` | Folder of the widget's files with `asset_source: storage`. |
| `text_file_formats` | `[.txt, .log]` | `advanced` only: extensions opened in the editor and followed live. |
| `actions_as_icons` | `false` | Actions as icons instead of labelled buttons. |
| `change_poll_interval` | `5s` | How often an open browser page asks for changed folders. |

```yaml
web_server:
  version: 3
  file_api:
    enable_format: true
  file_browser:
    variant: advanced
    asset_source: storage
    asset_path: /sdcard/file-explorer
    text_file_formats: [.txt, .log, .yaml]
```

### `web_server_idf`

Uploads are multipart; large uploads are streamed to the storage through the worker. No options of its own.
