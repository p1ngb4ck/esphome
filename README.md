# esphome-mipi-display

ESPHome `dev` plus the display changes of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`: MIPI
DSI on the ESP32-P4 (link options, generic commands, I2C init writes, video-only bridges, scanout), LVGL drawing into
the DSI frame buffers, LVGL RGB888, and touch drivers. One branch per change; `main` contains every branch. Base:
ESPHome `dev` at `1f70f86b47` with the two display repos merged as they are:

- [esphome-rpi-7inch-display](https://github.com/p1ngb4ck/esphome-rpi-7inch-display) `main`: ft5x06 vendor IDs, mipi
  `i2c_write` init entries, mipi_dsi link options (`frame_ack`, `continuous_clock`, `eotp`, `lp_horizontal_blanking`,
  `lp_vertical_active`), `command_mode: generic`, the model `RPI-7INCH-V1`. Its `pr/mipi-dsi-video-mode` is optional and
  not part of this `main`.
- [esphome-dfrobot-dfr0550-display](https://github.com/p1ngb4ck/esphome-dfrobot-dfr0550-display) `main`: ft5x06
  `firmware_reads`, i2c `bus_recovery`.

The code of the branches below is taken over unchanged from `testing_dev` (the lines of mipi_dsi, display, lvgl,
ft63x6, touchscreen assigned by `git blame`); where `testing_dev` adds lines next to lines the display repos wrote in
their reviewed form, the repos' form is kept.

## Branches

| Branch | Change | Chips |
|---|---|---|
| `pr/mipi-dsi-read-registers` | mipi_dsi `read_registers`: in generic command mode each listed 16-bit register read back after the init sequence and logged | ESP32-P4 |
| `pr/mipi-dsi-video-mode-only` | mipi_dsi `video_mode_only` (no DBI, no DCS commands; delays and I2C writes only), models `ECA20-MHA1-I` (DSI to HDMI) and `WAVESHARE-4.3-DSI` | ESP32-P4 |
| `pr/mipi-dsi-scanout` | mipi_dsi `frame_buffers` (up to 3), the frame buffers handed to a direct client and flipped at the frame end (event-driven flip wait), display scanout interface, LVGL drawing straight into the frame buffers when the display scans out of two, LVGL's PPA draw unit off | ESP32-P4 |
| `pr/lvgl-color-depth-32` | lvgl `color_depth: 32` as RGB888 (3 bytes per pixel) | ESP32-P4 |
| `pr/ft63x6-edt-m06` | ft63x6, touchscreen: EDT-M06 work-mode reads (`edt_read_variant`) | all |
| `pr/gsl3680` | gsl3680 touchscreen driver (JC8012P4A1) | all |

Every branch is ESPHome `dev` + both display repos + that one change. Not in this repo: the LVGL camera layer
(camera topic), canvas buffers, screen capture and i18n (media apps), the LVGL HID input
([esphome-hid](https://github.com/p1ngb4ck/esphome-hid)).

## Contributions

- gsl3680: from [kvj/esphome](https://github.com/kvj/esphome) `jd9365_gsl3680` via
  [NonaSuomy](https://github.com/NonaSuomy)'s esphomebrew; Silead's GPL notice kept in its files.

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-mipi-display@main
    components: [mipi, mipi_dsi, display, lvgl, ft5x06, ft63x6, touchscreen, i2c, gsl3680]
```

`external_components` replaces components only; the ESPHome core of the build host stays as installed. With the other
topics together use the combined branch `github://p1ngb4ck/esphome@topics-combined`.
