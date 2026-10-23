# mipi_dsi: model RPI-7INCH-V1 (Raspberry Pi 7" Touch Display v1)

800x480 DSI display with a TC358762 DSI-DPI bridge and an ATTINY88 (I2C 0x45) for power, resets and backlight.

The model's init sequence powers the panel through the ATTINY88 and releases the bridge reset (I2C entries), then
sets up the bridge (generic writes). Link: 1 lane, 720 Mbps, 30 MHz pixel clock, RGB888, no frame acknowledge,
continuous clock, no EoT packet, no low power in the horizontal blanking or the active lines.

Needs:
- [pr/mipi-dsi-link-options](https://github.com/p1ngb4ck/esphome/tree/pr/mipi-dsi-link-options)
- [pr/mipi-dsi-generic-commands](https://github.com/p1ngb4ck/esphome/tree/pr/mipi-dsi-generic-commands)
- [pr/mipi-i2c-init-writes](https://github.com/p1ngb4ck/esphome/tree/pr/mipi-i2c-init-writes)

```yaml
display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1
```

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

Part of [rpi7-v1-upstream](https://github.com/p1ngb4ck/esphome/tree/rpi7-v1-upstream).
