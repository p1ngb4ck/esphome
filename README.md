# ft5x06: vendor ID 0x79, invalid touch count

- Vendor ID 0x79 (Raspberry Pi 7" Touch Display v1) is accepted next to the three known ones; other vendor IDs
  still mark the touchscreen failed.
- A touch count above the maximum (some controllers read 0xFF until the first touch) is treated as no touch instead
  of logging "Failed to read status" on every update.

No configuration change.

Part of [rpi7-v1-upstream](https://github.com/p1ngb4ck/esphome/tree/rpi7-v1-upstream).
