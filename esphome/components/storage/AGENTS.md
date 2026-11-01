# storage: guide for AI agents

The storage interface: one registry for every storage, one path scheme, one asynchronous worker. Every component that
touches files (drivers, players, file managers, uploads, settings files) goes through it. The headers are the
contract: `storage.h`, `storage_worker.h`, `automation.h`, `transfer_buffer.h`. Never assume POSIX / stdio behaviour
beyond what they state. `.ai/storage-interface.md` maps the drivers, their capabilities and DMA.

## Rules that decide every design

- **Main loop vs task.** Control plane (registry, mount / unmount, format, connect) runs on the main loop only. Data
  plane calls (stat, list_dir, mkdir, rmdir, remove, rename, open / read / write) are blocking and task-agnostic;
  calls on one storage instance are serialized by the caller.
- **Bulk data goes through the worker** (`global_storage_worker`, `USE_STORAGE_WORKER`): copy, move, trees, raw
  transfers, format, mount, list_dir, file streams. On media with `STORAGE_CAP_IO_TASK_SAFE` (SD) it runs on the
  worker's task, otherwise sliced on the loop (one chunk per pass). **Completions always fire on the main loop.**
- **Blocking single operations** (stat, mkdir, remove, rename outside a move, small read_file / write_file) run on
  the caller's thread. First check `global_storage_worker->has_active_task_io(storage)`: when true the worker task is
  inside that medium and a second thread would enter the driver - refuse with `NOT_READY`, as every action does.
- **Never block the main loop with bulk I/O.** The blocking helpers `copy()`, `move()`, `read_file()`,
  `write_file()` are for small or boot-time use and are capped by `max_blocking_transfer_size`.
- **Deferred, not nested:** no storage call inside an LVGL event handler or another component's callback frame;
  `defer()` it to the next loop pass.
- **Memory:** path buffers are `STORAGE_PATH_MAX` (256) bytes each. Directory listings and other per-entry lists go
  to PSRAM (`RAMAllocator` / `ExternalRAMAllocator`), never into growing `std::vector`s in internal RAM.

## Paths

- Consumers use full VFS paths (`/sdcard/dir/file`). `global_storage_registry->resolve_path(vfs, &rel)` returns the
  `PathStorage*` and the driver-relative path (longest mount prefix at a `/` boundary; `""` is the mount point).
  Driver and worker calls take the relative path plus the storage; `StorageRegistry::build_path()` joins them back.
- Limits: `STORAGE_NAME_MAX` 255, `STORAGE_PATH_MAX` 256 (relative), `STORAGE_VFS_PATH_MAX` (full),
  `STORAGE_MAX_RECURSION_DEPTH` 4 (tree walks). Too long = `INVALID_ARGS`, never truncated.
- Roots for a browser: every registered path storage, `for_each_path_based(cb)`; `get_mount_path()`, `get_info()`
  (name, kind `sd` / `usb` / `nfs` / `iso` / `cdrom` / ..., total / free bytes, mounted / removable / read-only).
  Network shares: `get_storage_type() == STORAGE_TYPE_NETWORK`, `as_filesystem() == nullptr`. Every path list must
  handle local and network storages alike.

## storage.h

- `StorageError` + `error_to_string()`, `OpenMode`, `SeekMode`, `StorageInfo`, `FileStat` (basename, size, mtime
  (0 = unknown), is_dir), `StorageCaps` (`IO_TASK_SAFE`, `DMA_STREAM`, `DMA_D2D`).
- `Storage` (info, type, caps, format) -> `RawStorage` (read / write / erase / geometry), `KeyValueStorage`,
  `MountableStorage` (mount caps - gate every UI button on its bit; USB: unmount only = safe eject), `PathStorage`
  (stat, list_dir (callback, no `.` / `..`, unordered, return false = stop), mkdir, rmdir, remove, rename),
  `FilesystemStorage` (open / read / read_dma / write / seek / tell / sync), `NetworkStorage` (connect / disconnect /
  read_chunk / write_chunk / truncate, `write_done()` when a written file is complete), `OpticalStorage`
  (read-only, `is_emulated()`, `has_medium()`).
- `StorageRegistry`: `size()` / `get(i)`, `for_each*`, `resolve_path`, `build_path`, registered / unregistered /
  quiesce callbacks, the change feed (`note_dir_changed`, `note_parent_changed`, `change_seq`, `dir_change`; an
  8-entry ring, `USE_STORAGE_CHANGE_FEED`) - a UI notes its own blocking changes there.
- Free helpers: `exists`, `file_size`, `alloc_dma_capable`, `read_file` / `write_file` / `append_file`, `copy` /
  `move`, `remove_recursive`.

## StorageWorker (storage_worker.h)

A submit returns OK once queued (the callback fires exactly once, later, on the main loop) or an immediate error
without callback: `NOT_READY` = pool full (`max_pending`), `INVALID_ARGS` = path too long.
- `async_copy` / `async_move` (the worker decides file vs tree; `overwrite=false` -> `ALREADY_EXISTS`; a move on one
  storage is a rename), `async_copy_tree` / `async_move_tree`, `async_mount`, `async_format`.
- `async_list_dir(target, rel, entry_fn, ctx, cb)`: `entry_fn` runs on the task - `ctx` must be task-safe and
  untouched until `cb`; copy entries into your own PSRAM list.
- Raw: `async_raw_read`, `async_raw_write`, `async_raw_verify_file`, `async_raw_erase`.
- Progress: `get_transfer_status(job, &status)` until the completion is delivered; `set_progress_listener()` fires
  on the main loop at most every 250 ms while transfers move (no polling).
- Contention: `is_busy_with(storage)`, `has_active_task_io(storage)`.
- Streams (one file, caller-paced): `begin_read` / `read_chunk` / `end_read`, `begin_write` / `write_chunk` /
  `end_write`, `seek`, `tell`. One call at a time per stream - wait for its callback; the data stays valid until
  then; always `end_*`. Stream callbacks run on the main loop.
- Not in the worker: stat, mkdir, remove, rmdir, rename outside a move, get_info - blocking, with the
  `has_active_task_io()` guard.

## Helpers for UIs

- `browse.h` (`storage::browse`): a file manager model - state, PSRAM pages of 50 entries, sort, selection,
  clipboard, guarded make_dir / rename / delete / stat, paste through the worker.
- `text_doc.h` (`storage::text`): editor / log viewer - no buffer of its own (the text lives in the UI widget),
  reads borrow the transfer buffer, saves go to `<name>.tmp` then rename, a one-step undo in PSRAM.
- `bench.h`: a speed test in `<root>/.p4-speedtest` through the worker.
- `iso_image.*`, `iso9660.*`: ISO images as read-only storages (`iso_mount`), read only through the worker.

## TransferBuffer (transfer_buffer.h)

`global_transfer_buffer->try_acquire(need)` / `release()`: one owner at a time, PSRAM, DMA-capable on the ESP32-P4.
`nullptr` means "stream in chunks instead", never an error. Borrow it for one operation and give it back.

## Writing a driver

Derive from the matching class (`FilesystemStorage`, `NetworkStorage`, `RawStorage`, ...), register in `setup()`,
call `request_storage_device()` (and `request_storage_worker()` when the driver uses the worker) in `to_code()`,
report capabilities honestly (`IO_TASK_SAFE` only when the driver can be entered from the worker task), register
the mount path for the FAT volume count. Unregister when the medium goes away.
