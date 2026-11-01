# gsl3680: guide for AI agents

Silead GSL3680 touch controller (used on several ESP32-P4 panels). At start the controller gets its firmware (a
register table in the component) over I2C, then reports touches on the interrupt pin.

- The firmware table is large const data in flash; it is written in chunks during setup, never copied to RAM.
- Silead's GPL notice stays in the files; the driver came from kvj/esphome `jd9365_gsl3680`.
- Touches are read on the interrupt, not polled.
