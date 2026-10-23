# ft5x06: other FT5x06-family vendor IDs

- A vendor ID other than the three known ones no longer marks the touchscreen failed: a warning is logged and the
  FT5x06 register map is used (e.g. the Raspberry Pi 7" Touch Display v1, vendor ID 0x79).
- A touch count above the maximum (some controllers read 0xFF until the first touch) is treated as no touch instead
  of logging "Failed to read status" on every update.

No configuration change.

Part of [rpi7-v1-upstream](https://github.com/p1ngb4ck/esphome/tree/rpi7-v1-upstream).
