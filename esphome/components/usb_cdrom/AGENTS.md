# usb_cdrom: guide for AI agents

USB optical drives as read-only `OpticalStorage`s (ISO 9660), on top of `usb_storage`'s transport. Read
`esphome/components/storage/AGENTS.md` (section on optical storages) first.

- `usb_cdrom.*`: drive detection (SCSI peripheral type 5), media checks (TEST UNIT READY / GET EVENT STATUS) every
  `media_check_interval` while a drive is plugged in - drives report no media events over USB, so this is the only
  timed check, and it stops when no drive is present.
- Mount = load + index the ISO 9660 directory tree on the storage worker's task (the parser is shared with ISO image
  mounting in `storage/iso9660.*`); unmount = eject.
- Consumers read discs through the storage worker only (no blocking readers on the main loop).
- `eject` / `load` actions send START STOP UNIT.
