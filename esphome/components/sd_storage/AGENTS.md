# sd_storage: guide for AI agents

An SD card as a `FilesystemStorage` + `MountableStorage` of the storage interface (read
`esphome/components/storage/AGENTS.md` first).

- `sd_storage_base.*`: everything common - mount / unmount through ESP-IDF's FatFs VFS, card detect, the triggers,
  file handles, the optional `file_system` choice (exFAT builds only, `USE_STORAGE_FILE_SYSTEM_SELECT`).
- `sd_storage.*` (`SdMmc`): the SD-MMC host (S3, S31, P4). Reports `IO_TASK_SAFE | DMA_STREAM`: the storage worker
  may run its I/O on its task, and DMA-capable buffers stream straight to the card.
- `sd_storage_spi.*` (`SdSpi`): SD over SPI. `IO_TASK_SAFE` only with `assume_exclusive_bus: true` (validated: the
  card must be the only device on its bus), because another device's transfer from the main loop would otherwise
  collide with the worker task.
- `power_ldo` (`SdMmc`, `USE_SD_STORAGE_POWER_LDO`): the esp_ldo channel that powers the card. When it is off at
  `setup()`, the first mount waits for its turn-on callback and `can_proceed()` holds the components after it; the
  core keeps running the scheduler meanwhile, so `esp_ldo.turn_off` / `delay` / `esp_ldo.turn_on` in an on_boot
  before priority 800 power-cycles the card without blocking. A failed first mount power-cycles the card once
  (`power_cycle_retry_()`: LDO off, `POWER_CYCLE_OFF_MS` by a scheduler timeout, on, mount again), held the same way.
- `automation.h`: mount / unmount / list_files actions, `is_mounted` condition.

Rules:
- Mount, unmount and card-detect handling run on the main loop; data I/O may come from the worker task.
- A removed card unregisters its storage; consumers learn it from the registry callbacks, never by polling.
- FAT volume slots are counted from the configured mount paths at build time; a new mount path must be registered
  in `to_code()`.
