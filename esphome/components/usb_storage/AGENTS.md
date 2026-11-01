# usb_storage: guide for AI agents

USB mass storage as FAT / exFAT storages. Read `esphome/components/usb_host/AGENTS.md` (the client model) and
`esphome/components/storage/AGENTS.md` (the storage contract) first.

- `usb_storage.*`: `USBStorage` client (mass-storage class, bulk-only transport) and one storage per plugged drive;
  each drive gets its entry's mount path or `/usb`, numbered when taken (`/usb2` ... `/usb9`).
- `msc_bot.h`: bulk-only transport (CBW / CSW) and the SCSI commands (INQUIRY, READ CAPACITY, READ / WRITE 10,
  TEST UNIT READY, REQUEST SENSE).
- `usb_storage_diskio.*`: the FatFs diskio bridge (sector reads / writes through the client's transfers).
- `automation.h`: remount / unmount / list_files actions, `is_mounted`.

Rules:
- A drive is mounted after enumeration on the main loop; sector I/O runs on the caller's thread (the storage worker
  task) and waits for its USB transfers. The storage reports `IO_TASK_SAFE`.
- Unmount = safe eject: the worker drains, the file system is synced, then the drive is released. Unplugging
  without unmount unregisters the storage at once; consumers learn it from the registry.
- `max_packet_size` of `usb_host` must hold a full-speed / high-speed bulk packet (512 on high-speed) or sector
  transfers are refused.
- `usb_cdrom` builds on this client for optical drives (SCSI MMC); keep shared SCSI code here.
