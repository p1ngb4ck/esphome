# esphome-mipi-display

MIPI DSI displays and touch controllers on the ESP32-P4: link options for bridge chips and difficult panels, generic
command mode, I2C writes in init sequences, video-only bridges (DSI to HDMI), up to three frame buffers with a
scanout interface that LVGL and video writers draw into directly, LVGL in RGB888, and touch drivers (GSL3680,
EDT-M06 variants, FT5x06 fixes, I2C bus recovery).

It merges [esphome-rpi-7inch-display](https://github.com/p1ngb4ck/esphome-rpi-7inch-display) (the Raspberry Pi 7"
Touch Display v1) and [esphome-dfrobot-dfr0550-display](https://github.com/p1ngb4ck/esphome-dfrobot-dfr0550-display)
(FT5x06 firmware reads, I2C bus recovery).

## Getting started

A DSI panel with two frame buffers, LVGL drawing straight into them:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-mipi-display@main
    components: [mipi, mipi_dsi, display, lvgl]

display:
  - platform: mipi_dsi
    id: main_display
    model: WAVESHARE-4.3-DSI
    frame_buffers: 2

lvgl:
  displays: [main_display]
```

A GSL3680 touch controller:

```yaml
touchscreen:
  - platform: gsl3680
    interrupt_pin: GPIO21
    reset_pin: GPIO22
```

Use your board's pins and your panel's model.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every new option - DSI link options, command modes, register read-back,
video-only bridges and their models, frame buffers and scanout, LVGL `color_depth: 32`, the touch options.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47` with the two display
repositories above merged.

| Branch | Change | Chips |
|---|---|---|
| `pr/mipi-dsi-read-registers` | mipi_dsi `read_registers`: in generic command mode each listed 16-bit register read back after the init sequence and logged | ESP32-P4 |
| `pr/mipi-dsi-video-mode-only` | mipi_dsi `video_mode_only` (no DBI / DCS commands; delays and I2C writes only), models `ECA20-MHA1-I` (DSI to HDMI) and `WAVESHARE-4.3-DSI` | ESP32-P4 |
| `pr/mipi-dsi-scanout` | mipi_dsi `frame_buffers` (up to 3), the frame buffers handed to a direct client and flipped at the frame end, the scanout interface, LVGL drawing straight into the frame buffers with two of them | ESP32-P4 |
| `pr/lvgl-color-depth-32` | lvgl `color_depth: 32` as RGB888 (3 bytes per pixel) | ESP32-P4 |
| `pr/ft63x6-edt-m06` | ft63x6, touchscreen: EDT-M06 work-mode reads (`edt_read_variant`) | all |
| `pr/gsl3680` | gsl3680 touchscreen driver | all |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).
- Third-party code keeps its own licence:
  - `gsl3680/`: the GSL3680 firmware and driver parts carry Silead's GPL notice.

## Thanks

The GSL3680 driver comes from Konstantin's ([kvj](https://github.com/kvj/esphome)) `jd9365_gsl3680`, which
reached this repository through [NonaSuomy](https://github.com/NonaSuomy)'s esphomebrew - thank you both. Silead's
GPL notice stays in its files. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
