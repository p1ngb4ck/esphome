# Storage interface — capability map (merkliste)

Source-anchored map of what `esphome/components/storage` already provides, so a session does NOT
re-derive or underestimate it. Line numbers are anchors at time of writing; re-grep if they drift.
Everything here is our own code unless a claim is tagged with an esp-idf source.

## Files
- `storage.h` / `storage.cpp` — abstractions, registry, free helper functions.
- `storage_worker.h` / `.cpp` — async worker (transfers + streaming), own FreeRTOS task on task-safe media.
- `transfer_buffer.h` / `.cpp` — optional DMA-capable PSRAM staging arena.
- `automation.{h,cpp}`, `fatfs_select.h`, `__init__.py` (codegen/defines).
- `browse.h` — header-only browse model for file manager UIs (`storage::browse`): one page of a
  folder at a time (PSRAM, the rest counted), sort within the page, selection, clipboard + paste via the
  worker (copy/move, "already there" asked about), guarded single calls (mkdir/rename/delete/stat).

## Core abstractions (`storage.h`)
- `Storage` (base): `get_info`, `get_storage_type`, `get_capabilities`, `format`.
- Four subtypes (no-RTTI downcast via `get_storage_type()` + `as_filesystem()`/`as_mountable()`):
  - `RawStorage` — offset byte access `read/write/erase` + `get_raw_geometry` (flash/FRAM/EEPROM). storage.h:244
  - `KeyValueStorage` — `get/set/erase/has/get_size/list_keys/ensure_initialized` (NVS). storage.h:287
  - `PathStorage` — `stat/list_dir/mkdir/rmdir/remove/rename` + `resolve`/mount path. storage.h:333
    - `FilesystemStorage` — `mount/unmount/sync/open/close/read/write/seek/tell`. storage.h:374
    - `NetworkStorage` — stateless `connect/disconnect/read_chunk/write_chunk/truncate`. storage.h:394
  - `MountableStorage` mixin — removable media (SD/USB/network). storage.h:329
- Data structs: `StorageInfo` (incl. `block_size`, `kind`, mounted/removable/read_only) storage.h:68;
  `FileStat` storage.h:138; `FileHandle` (holds `FILE *file`) storage.h:150; `RawGeometry`/`RawEraseCaps` storage.h:218.
- Partial-read contract: OK + `*bytes_transferred < len` = EOF, not error. storage.h:248

## Capability model — `StorageCaps` bitmask, `get_capabilities()` (storage.h:160, :206)
- `STORAGE_CAP_IO_TASK_SAFE = 1<<0` — data-plane may run on the worker task. storage.h:164
- `STORAGE_CAP_DMA_STREAM   = 1<<1` — read()/read_chunk() land bytes in a DMA-capable caller buffer
  by DMA (no bounce). Hint for consumers to allocate the destination DMA-capable. (added this effort)
- `STORAGE_CAP_DMA_D2D      = 1<<2` — device→device DMA, no intermediate buffer, via an endpoint
  (not yet implemented; no driver can honor it — SDMMC/USB DMA to memory, not peripheral→peripheral).
- Default 0: a driver opts in only after a verified real path. Contract: never advertise a cap the
  code does not honor.

## StorageRegistry (`storage.h:441`, `storage.cpp`)
- `resolve_path(vfs_path, &rel)` longest-prefix mount match; `build_path`. storage.h:513
- `for_each[_filesystem/_raw/_kv/_network/_path_based]`, `get(i)`/`size()`. storage.h:497
- `register/unregister/quiesce_storage` with drain contract (no in-flight call on return);
  `is_registered`; hotplug callbacks `on_registered/unregistered/quiesce`. storage.h:466
- Optional dir-change feed (`USE_STORAGE_CHANGE_FEED`). storage.h:531
- `global_storage_registry`.

## StorageWorker (`storage_worker.h:455`) — async, `USE_STORAGE_WORKER`
PollingComponent (5 ms); own FreeRTOS task on task-safe media (`USE_STORAGE_WORKER_TASK`), else
loop-sliced. Two engines share one queue. Completions ALWAYS fire on the main loop.
- Transfers: `async_copy/move`, `async_copy_tree/move_tree` (engine walks the tree),
  `async_mount`, `async_format`. storage_worker.h:490-515
- Raw jobs: `async_raw_read/write/verify_file/erase` (erase→write→verify phases, sector-aligned,
  pseudo-erase 0xFF for media w/o erase). storage_worker.h:522-541
- Streaming API (one pooled `StreamHandle`, sequenced by completions):
  `begin_read/read_chunk/end_read`, `begin_write/write_chunk/end_write`, `seek`, `tell`.
  storage_worker.h:573-608
- Progress event: `set_progress_listener(cb, ctx)` -- one listener, called on the main loop from
  update() at most every 250 ms when running transfers moved (or one started/ended); no polling needed.
- Status/contention: `get_transfer_status` (progress bars), `is_busy_with`, `has_active_task_io`
  (cross-engine serialization — never two threads in one medium). storage_worker.h:549-566
- Chunk buffers: `chunk_buffer_()` (storage_worker.cpp:1190) allocs via `alloc_dma_capable` —
  `chunk_buf_task_`/`chunk_buf_loop_` (storage_worker.h:717-718).
- Stream READ step: `StreamState::READING` passes `pending_read_buf` STRAIGHT to
  `driver->read()/read_chunk()`. storage_worker.cpp:2321-2329. (This is where cap-driven DMA
  routing on the existing read path belongs.)
- `global_storage_worker`.

## OpticalStorage (`storage.h`, `storage.cpp`) - emulated and real CD/DVD drives
- `OpticalStorage : FilesystemStorage, MountableStorage`: a FilesystemStorage (STORAGE_TYPE_FILESYSTEM), so every
  consumer of the interface and the worker use it like any filesystem; `as_optical()` is the no-RTTI downcast for
  what only optical drives have: `is_emulated()`, `has_medium()`. Read-only: write / mkdir / rmdir / remove /
  rename PERMISSION_DENIED, open other than READ PERMISSION_DENIED, sync OK.
- The base implements open / close / read / seek / tell on up to MAX_OPEN_FILES (4) handles
  (`OpticalFileHandle`: start, size, pos on the medium, the medium's own handle); a driver supplies
  `find_extent_(path)` (where the file lies on the medium), `read_medium_(file, offset, ...)` and optionally
  `open_medium_` / `close_medium_`.
- Emulated (ISO image): the medium is the image file - `open_medium_` opens it on its storage (FilesystemStorage) or
  nothing (NetworkStorage), `read_medium_` seeks + reads it (or read_chunk at the offset) on the caller's thread,
  i.e. on the worker when the consumer reads through the worker. `get_backing_storage()` = the image's storage.
- Real (usb_cdrom): `read_medium_` reads sectors by SCSI.
- Worker: serialization by medium - overlaps compare `medium_of()` (the backing if any), is_busy_with /
  has_active_task_io / the drain match a storage directly or through its backing.

## ISO 9660 (`iso9660.h/.cpp`, `USE_STORAGE_ISO9660`)
- `Index` (PSRAM, grown with RAMAllocator ALLOC_EXTERNAL, limit `INDEX_MAX` 2 MB): directory table (16 B per
  directory) + entries (18 B + UTF-8 name); `lookup` / `stat` / `list` (ASCII case-insensitive), volume label
  and size. No I/O.
- `Builder`: `start(index, session_lba, max_sectors)`, `next(&lba, &count)` -> READ / DONE / FAILED,
  `feed(data, count)`. Volume descriptors from session + 16, Joliet SVD preferred over the PVD, 2048-byte
  logical blocks only, directories breadth first; multi-extent files merged when contiguous (else open ->
  NOT_SUPPORTED), interleaved files the same. No Rock Ridge, no UDF.
- Used by the ISO mount (below) and by usb_cdrom.

## ISO mount (`iso_image.h/.cpp`, `USE_STORAGE_ISO_MOUNT`)
- YAML `storage: iso_mount: {mount_path: /iso, max_images: 1}`; `IsoMounter` (component) holds the slots
  (`/iso`, `/iso2` .. `/iso9`). Actions `storage.iso_mount` (file, on_complete) and `storage.iso_unmount`
  (image file or mount path, on_complete).
- `IsoImageStorage` (an emulated OpticalStorage, made in PSRAM per mount, freed after unmount): the image is read
  only through worker streams (`begin_read` / `seek` / `read_chunk` / `end_read`, main loop chain, buffer = 64 KB
  of the transfer buffer or one 2 KB PSRAM sector during the mount); `stat` / `list_dir` from the index; file data
  through the OpticalStorage handles (the image read on the caller's thread through its storage).
- FileManager (panel UI): tapping an .iso file asks "Mount <name>?" and mounts it (a FileManager job); the drive
  appears among the roots with the disc icon; eject = the unmount button.
- Unmount: the actions, `unmount()` (FileManager, file_api), and by itself when the backing storage is
  quiesced / unregistered (registry quiesce callback: detached at once, unmounted on the next loop pass).
  An image on an image is refused. Caps = the backing's IO_TASK_SAFE.
- web_server file_api: `/files/mount?path=<image file>` mounts the image (`global_iso_mounter`), the reply
  acknowledges the start like any async mount; `/files/unmount?path=/iso` unmounts it.

## usb_cdrom (`components/usb_cdrom`, `USE_USB_CDROM`)
- On usb_storage: its client enumerates and sends INQUIRY; peripheral type 5 (CD/DVD) is handed to usb_cdrom
  (`USBCdrom::attach`) instead of being refused - no 5 s ready check, no FAT. `CdromDrive` (PSRAM, made at
  connect, freed at disconnect; a real OpticalStorage) is a friend of USBStorageDevice and uses its BOT transport under `io_lock_`:
  TEST UNIT READY / REQUEST SENSE, GET EVENT STATUS NOTIFICATION (media, polled), GET CONFIGURATION (profile),
  READ TOC format 1 (last session), READ CAPACITY, READ(10) with 2048-byte sectors, PREVENT ALLOW MEDIUM REMOVAL,
  START STOP UNIT (eject / load, IMMED).
- Registered at connect (unmounted) under `/cdrom` (`/cdrom2` ..), caps MOUNT | UNMOUNT, IO_TASK_SAFE.
  `mount()` (worker task via async_mount): TEST UNIT READY, index built with SCSI reads (64 KB of the transfer
  buffer or the 2 KB sector buffer), tray locked when media events work; a user mount on an empty drive loads
  the tray. `unmount()` (main loop) = safe eject: quiesce, index freed, tray unlocked, eject. Refused while the
  worker task is inside the drive.
- `read_medium_`: whole sectors straight into the caller's buffer (`scsi_read`), partial sectors through the 2 KB
  PSRAM sector buffer (also a one-sector cache); `stat` / `list_dir` from the index.
- Media change: BOT has no interrupt endpoint and MMC media events are polled (as Linux sr does), so
  `media_check_interval` (default 2 s, 0 = off) runs only while a drive is plugged, skipped while the worker uses
  the drive or `io_lock_` is taken. New medium -> async_mount; removal -> quiesce + index freed; eject request
  (button with the tray locked) -> safe eject. Triggers `on_media_mounted` / `on_media_removed` (mount path) from
  the component's loop, enabled from the task. Actions `usb_cdrom.eject` / `usb_cdrom.load` (mount_path optional).

## TransferBuffer (`transfer_buffer.h`) — optional, `USE_STORAGE_TRANSFER_BUFFER`
- Single-owner PSRAM staging arena; `try_acquire(need)`/`release()`; `capacity()`; `is_dma_capable()`.
- DMA-capable ONLY on S3/P4 (`MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA`), else plain external RAM
  (memcpy staging). transfer_buffer.cpp:49-55. Purely additive: absent/busy/small → consumer streams.
- `global_transfer_buffer`.

## Free helpers (`storage.cpp`)
- `read_file/write_file/append_file` (fs + network overloads + PathStorage dispatch); `copy`/`move`
  (tree-aware, self-copy guarded); `remove_recursive`; `exists`; `file_size`. storage.cpp:459-1086
- `alloc_dma_capable(want, on_task, &actual)` — storage.cpp:904: on S3/P4 + on_task → 64 kB (P4) /
  32 kB (S3) `MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA`; else internal `MALLOC_CAP_INTERNAL|DMA|8BIT`,
  halving to 4 kB floor. The blocking copy paths use `alloc_copy_chunk` (on_task=false → internal).
- `error_to_string`/`error_from_errno`.

## DMA reality (verified this effort)
- Worker READ path carries the caller buffer unchanged to the driver. storage_worker.cpp:2321-2329, :2557-2579
- FATFS `f_read` reads whole contiguous sectors DIRECTLY into the app buffer via `disk_read`
  (clipped at cluster boundary); partial head/tail sector bounces via `fp->buf`+memcpy.
  [esp-idf v5.5 clone components/fatfs/src/ff.c:4001-4006]
- SDMMC `sdmmc_read_sectors` DMAs straight into the caller `dst` when the host alignment check
  passes; on P4 a PSRAM `dst` is allowed (`SOC_SDMMC_PSRAM_DMA_CAPABLE` gates the external-RAM
  exclusion); else bounce via `MALLOC_CAP_DMA` temp + memcpy.
  [esp-idf v5.5 components/sdmmc/sdmmc_cmd.c, sdmmc_read_sectors/_dma]
- Drivers TODAY (our code) are all CPU-copy on read:
  - sd_storage: `fopen`/`fread` (stdio FILE buffer + FATFS window). sd_storage_base.cpp:297,:331
  - usb_storage: `fread` → our diskio → SCSI READ(10) over USB MSC; URB buffer `memcpy`'d to dest.
    usb_storage.cpp:205; usb_storage_diskio.cpp:39-43
  - nfs_client/ftp_client: TCP socket `recv` (lwIP copy). nfs_client.cpp:617; ftp_client.cpp:631
  - binary_storage: SPI/I2C/mmap small media.
- → Only SD-MMC on P4 can get a real DMA read (new FatFs-direct `FIL`/`f_read` path + DMA-capable
  aligned PSRAM buffer). USB MSC copy is structural. D2D infeasible for all.

## DMA work plan (strict order)
1. Storage IF: cap bits (done) + existing read/copy methods AUTOMATICALLY route to DMA by the
   reported caps (worker READING step + copy engine), additive fallback to the CPU path.
2. Device drivers: SD gets a FatFs-direct DMA read path, then advertises `STORAGE_CAP_DMA_STREAM`.
   Others keep CPU-copy caps (verified they cannot DMA).
3. Consumers: simple_video_player (and others) — read directly into a DMA-capable buffer; drop any
   arena→ring double copy.

## Hard rules reminder (CLAUDE.md)
ESP32-P4, ESP-IDF (NOT POSIX — no stdio-semantics assumptions). Verify behavior in real source +
cite it. PSRAM for anything >few kB with hard-fail. No ESP_LOG / fixed blocking waits in the video
hot path. Prefer existing components over hand-rolling.
