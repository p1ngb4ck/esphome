# Raspberry Pi 7" Touch Display v1 on ESP32-P4: DSI display (800x480, TC358762 bridge) and touch

ESPHome `dev` plus all changes this display needs. The display powers up through its ATTINY88 (I2C 0x45) from the
model's init sequence, once the DSI lanes are up: no `on_boot` lambda, the picture comes up after a power cycle, a
reset and an OTA restart alike. Each change as its own pull request branch:

- [pr/ft5x06-vendor-ids](https://github.com/p1ngb4ck/esphome/tree/pr/ft5x06-vendor-ids): ft5x06 accepts vendor ID 0x79 and treats an invalid touch
  count as no touch.
- [pr/mipi-dsi-link-options](https://github.com/p1ngb4ck/esphome/tree/pr/mipi-dsi-link-options): mipi_dsi options `frame_ack`, `continuous_clock`, `eotp`,
  `lp_horizontal_blanking`, `lp_vertical_active`.
- [pr/mipi-dsi-generic-commands](https://github.com/p1ngb4ck/esphome/tree/pr/mipi-dsi-generic-commands): mipi_dsi `command_mode: generic` for bridge
  chips (generic writes, no acknowledge, no DCS soft reset or added DCS commands).
- [pr/mipi-i2c-init-writes](https://github.com/p1ngb4ck/esphome/tree/pr/mipi-i2c-init-writes): I2C register writes (`i2c_write`) as init sequence
  entries, display option `i2c_id`.
- [pr/mipi-dsi-rpi-7inch-v1](https://github.com/p1ngb4ck/esphome/tree/pr/mipi-dsi-rpi-7inch-v1): the model `RPI-7INCH-V1` (needs the three mipi PRs above).
- `main` also: with `command_mode: generic` the init sequence goes out before `esp_lcd_panel_init()` starts the
  video, the bridge's register writes in command mode (testing_dev a1f865b6f3).
- Optional, not in `main`: [pr/mipi-dsi-video-mode](https://github.com/p1ngb4ck/esphome-rpi-7inch-display/tree/pr/mipi-dsi-video-mode):
  mipi_dsi `video_mode` (`burst`, `non_burst_sync_pulses`, `non_burst_sync_events`), `RPI-7INCH-V1` with
  `non_burst_sync_pulses`; the display works without it.

## Status

- Init: complete, after a cold start, a restart and a flash (OTA) alike
- Touch: complete
- Backlight: complete (ATTINY88 register 0x86, 0-255)
- Checks: upstream's CI jobs run locally (not on GitHub) and an own review; no open findings

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome@rpi7-v1-upstream
    components: [mipi, mipi_dsi, ft5x06]

display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1

touchscreen:
  - platform: ft5x06
    address: 0x38
```

`i2c_id` on the display selects the I2C bus of the display connector when there is more than one. The backlight is
ATTINY88 register 0x86 (0-255), see the example.

## Orientation

The display mirrors its picture in hardware through the scan direction of its panel (set by the ATTINY88 at start-up),
with ESPHome's `transform` option:

| `mirror_x` | `mirror_y` | Picture |
|---|---|---|
| `false` | `false` | normal (default) |
| `true` | `false` | mirrored left / right |
| `false` | `true` | mirrored top / bottom |
| `true` | `true` | rotated 180 degrees |

Rotated 180 degrees:

```yaml
display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1
    transform:
      mirror_x: true
      mirror_y: true

touchscreen:
  - platform: ft5x06
    address: 0x38
    transform:
      mirror_x: true
      mirror_y: true
```

The touch controller is not affected by the panel's scan direction: give the touchscreen the same `transform` as the
display. 90 and 270 degrees are not possible in hardware; `rotation` on the display (or in LVGL) rotates in
software.

## Example

[example/rpi-7inch-v1.yaml](example/rpi-7inch-v1.yaml), tested with the Guition JC-ESP32P4-M3-Dev; should work on any
ESP32-P4 board with a DSI connector. Set for your board before use:

- `esp32:` `board:`, and `engineering_sample: true` for ESP32-P4 chip revisions below v3 (boot log: `chip revision: vX.Y`)
- `i2c:` your board's pins on the display connector
- `esp32_hosted:` your board's pins (`reset_pin`, `cmd_pin`, `clk_pin`, `d0_pin`-`d3_pin`, `active_high`)

```yaml
# Raspberry Pi 7" Touch Display v1 on an ESP32-P4: minimal config, ESPHome defaults, the display parts from the
# branch rpi7-v1-upstream. Shows the display's test card. Tested with the Guition JC-ESP32P4-M3-Dev; should work on
# any ESP32-P4 board with a DSI connector (set the board-specific options, see README).

esphome:
  name: esp32-p4-rpi7-generic
  friendly_name: ESP32-P4-RPi7-Generic

esp32:
  variant: ESP32P4
  flash_size: 16MB
  framework:
    type: esp-idf

external_components:
  - source: github://p1ngb4ck/esphome@rpi7-v1-upstream
    components:
      - mipi      # changed upstream component: i2c_write in init sequences
      - mipi_dsi  # changed upstream component: link options, generic commands, I2C init writes, model RPI-7INCH-V1
      - ft5x06    # changed upstream component: vendor ID 0x79, 0xFF touch count
    refresh: always

psram:
  mode: hex
  speed: 200MHz

# MIPI DSI PHY supply
esp_ldo:
  - channel: 3
    voltage: 2.5V

# Wi-Fi through the ESP32-C6: add your board's pins (see README)
esp32_hosted:
  variant: ESP32C6

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret esphome_encryption_key

ota:
  - platform: esphome

logger:

i2c:  # your board's I2C pins on the display connector
  sda: GPIO7
  scl: GPIO8

i2c_device:
  - id: attiny
    address: 0x45

display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1
    transform:
      mirror_x: true
      mirror_y: true
    show_test_card: true

touchscreen:
  - platform: ft5x06
    address: 0x38
    transform:
      mirror_x: true
      mirror_y: true

output:
  - platform: template
    id: backlight_output
    type: float
    write_action:
      - lambda: id(attiny).write_byte(0x86, static_cast<uint8_t>(state * 255.0f));  # PWM

light:
  - platform: monochromatic
    name: Backlight
    output: backlight_output
    restore_mode: ALWAYS_ON
```

## License

ESPHome's files: [LICENSE](LICENSE). The additions of this branch (the changes to `mipi`, `mipi_dsi` and `ft5x06`,
the model file `esphome/components/mipi_dsi/models/raspberrypi.py`, the example and this README):
[LICENSE-MIT](LICENSE-MIT).
