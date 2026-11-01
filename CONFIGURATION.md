# Configuration reference

New and changed options; everything else is as documented on [esphome.io](https://esphome.io/components/display/).
The first steps are in [README.md](README.md).

## `mipi_dsi` display

| Option | Default | Description |
|---|---|---|
| `frame_buffers` | `1` | Frame buffers in PSRAM, 1-3. With 2 or more the display scans out of one while LVGL (or a video writer) draws into the other; the buffers flip at the frame end. |
| `video_mode` | `burst` (or the model's) | `burst`, `non_burst_sync_pulses`, `non_burst_sync_events`. |
| `frame_ack` | `true` | Wait for the DSI host's frame acknowledge. |
| `continuous_clock` | `false` | Keep the DSI clock lane in high-speed mode between frames (bridges that need it). |
| `eotp` | `true` | Send the end-of-transmission packet. |
| `lp_horizontal_blanking` / `lp_vertical_active` | `true` / `true` | Low-power mode in the blanking periods. |
| `command_mode` | `dcs` | `dcs`, or `generic` for bridge chips: generic writes, no acknowledge, no DCS soft reset or added DCS commands; the init sequence goes out before the video starts. |
| `read_registers` | `[]` | `generic` mode: up to 32 16-bit registers read back after the init sequence and logged. |
| `video_mode_only` | `false` | No DBI / DCS commands at all (delays and I2C writes only) - for video-only bridges. |
| `i2c_id` | the only bus | The I2C bus a model's init sequence writes to (bridge or backlight controller on the display connector). |

Models added: `RPI-7INCH-V1` (Raspberry Pi 7" Touch Display v1, esphome-rpi-7inch-display), `ECA20-MHA1-I` (DSI to
HDMI bridge), `WAVESHARE-4.3-DSI`. Model files can put I2C register writes into their init sequence
(`i2c_write(address, register, value)` in `esphome/components/mipi`).

```yaml
display:
  - platform: mipi_dsi
    id: main_display
    model: ECA20-MHA1-I
    frame_buffers: 2
    continuous_clock: true
```

## LVGL

| Option | Default | Description |
|---|---|---|
| `color_depth` | `16` | `32` = RGB888 (3 bytes per pixel) for displays configured for 24-bit colour. |

With a display of 2+ `frame_buffers`, LVGL renders straight into the frame buffers (no copy) and LVGL's PPA draw unit
is off.

## Touch

| Platform / component | Option | Default | Description |
|---|---|---|---|
| `gsl3680` touchscreen | `interrupt_pin`, `reset_pin` | required | Silead GSL3680 (with its firmware), plus the usual touchscreen and I2C options. |
| `ft5x06` touchscreen | `firmware_reads` | `false` | Read touches as the controller's firmware reports them (per-touch event status, no setup wait) - DFRobot DFR0550 and similar. |
| every touchscreen (`ft63x6`) | `edt_read_variant` | `false` | EDT-M06 work-mode reads. |
| `ft5x06` | - | - | Vendor ID 0x79 accepted; an invalid touch count is no touch. |
| `i2c` | `bus_recovery` | - | `true`: recover a stuck bus before use (Arduino and ESP-IDF). |

```yaml
touchscreen:
  - platform: gsl3680
    id: touch
    interrupt_pin: GPIO21
    reset_pin: GPIO22
    transform:
      swap_xy: true
```
