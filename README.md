# Raspberry Pi 7" Touch Display v1 on ESP32-P4: DSI display (800x480, TC358762 bridge) and touch

ESPHome `dev` plus what this display needs, as one commit. Split for upstream:

| Branch | Change |
|---|---|
| `pr/ft5x06-vendor-ids` | ft5x06: other FT5x06-family vendor IDs; 0xFF touch count = no touch |
| `pr/mipi-dsi-link-options` | mipi_dsi: `frame_ack`, `continuous_clock`, `eotp`, `lp_horizontal_blanking`, `lp_vertical_active` |
| `pr/mipi-dsi-generic-commands` | mipi_dsi: `command_mode: generic` (generic writes, no ack, no DCS soft reset / commands) |
| `pr/mipi-dsi-rpi-7inch-v1` | mipi_dsi: model `RPI-7INCH-V1` (needs the two above) |

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome@rpi7-v1-upstream
    components:
      - mipi_dsi  # changed upstream component: link options, generic commands, model RPI-7INCH-V1
      - ft5x06    # changed upstream component: other FT5x06-family vendor IDs, 0xFF touch count

esphome:
  on_boot:
    # ATTINY88 (0x45): powers the panel and releases the bridge reset before the display (600) sets up.
    # Blocking on purpose: with delay actions the display would set up first.
    - priority: 850
      then:
        - lambda: |-
            auto write = [](uint8_t reg, uint8_t value, uint32_t ms) {
              id(attiny).write_byte(reg, value);
              delay(ms);
            };
            write(0x85, 0x00, 30);   // POWERON off
            write(0x86, 0x00, 8);    // PWM off
            write(0x83, 0x00, 8);    // PORTC: bridge, LCD and touch in reset
            write(0x81, 0x04, 8);    // PORTA: LCD_LR (0x08 LCD_UD: panel rotated 180 degrees)
            write(0x82, 0x80, 8);    // PORTB: LCD_MAIN
            write(0x83, 0x01, 88);   // PORTC: LED_EN
            write(0x83, 0x0D, 8);    // PORTC: RST_LCD_N, RST_BRIDGE_N released
            write(0x8D, 0x04, 8);    // bridge register 0x047C = 0
            write(0x8C, 0x7C, 8);
            write(0x90, 0x00, 8);
            write(0x91, 0x00, 108);
            write(0x83, 0x0F, 8);    // PORTC: RST_TP_N released

i2c_device:
  - id: attiny
    address: 0x45

display:
  - platform: mipi_dsi
    model: RPI-7INCH-V1

touchscreen:
  - platform: ft5x06
    address: 0x38

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
```

Complete example: [example/rpi-7inch-v1.yaml](example/rpi-7inch-v1.yaml). Tested with the Guition JC-ESP32P4-M3-Dev;
should work on any ESP32-P4 board with a DSI connector.
Set for your board before use:

- `esp32:` `board:`, and `engineering_sample: true` for ESP32-P4 chip revisions below v3 (boot log: `chip revision: vX.Y`)
- `i2c:` your board's pins on the display connector
- `esp32_hosted:` your board's pins (`reset_pin`, `cmd_pin`, `clk_pin`, `d0_pin`-`d3_pin`, `active_high`)

## License

ESPHome's files: [LICENSE](LICENSE). The model file
`esphome/components/mipi_dsi/models/raspberrypi.py`, the example and this README: [LICENSE-MIT](LICENSE-MIT).
