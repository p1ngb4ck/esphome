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
- `automation.h`: mount / unmount / list_files actions, `is_mounted` condition.

Rules:
- Mount, unmount and card-detect handling run on the main loop; data I/O may come from the worker task.
- A removed card unregisters its storage; consumers learn it from the registry callbacks, never by polling.
- FAT volume slots are counted from the configured mount paths at build time; a new mount path must be registered
  in `to_code()`.
