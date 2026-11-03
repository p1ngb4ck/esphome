# mipi_dsi (scanout): guide for AI agents

The additions to upstream's MIPI DSI driver (esphome-mipi-display).

- Link options (`frame_ack`, `continuous_clock`, `eotp`, `lp_*`, `video_mode`), `command_mode: generic` (bridge
  chips: the init sequence before the video starts, generic writes), `read_registers`, `video_mode_only`, I2C writes
  in init sequences (`mipi.i2c_write()` in model files, `i2c_id`).
- Scanout: `frame_buffers` (1-3, PSRAM). The display hands its frame buffers to one direct client at a time (LVGL or
  a video writer); a flip happens at the frame end, and waiting for it is event-driven (the DSI frame-done callback),
  never a delay loop. With two buffers LVGL renders straight into the back buffer and LVGL's PPA draw unit is off.
- New panels are model files in `models/`; a model carries its init sequence, timings, link options and defaults.
- The frame buffers are the largest allocations on the device: never add a buffer beside them - writers use the
  scanout interface.
