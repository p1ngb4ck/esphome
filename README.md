# DFRobot DFR0550 5" DSI display on ESP32-P4

ESPHome `dev` plus all changes this display needs. Each change as its own pull request branch:

- [pr/i2c-bus-recovery-option](https://github.com/p1ngb4ck/esphome/tree/pr/i2c-bus-recovery-option): `i2c` option `bus_recovery` (default `true`);
  `false` skips the bus recovery at setup.
- [pr/ft5x06-firmware-reads](https://github.com/p1ngb4ck/esphome/tree/pr/ft5x06-firmware-reads): `ft5x06` option `firmware_reads` for the display's
  emulated touch controller.

The display's STM32 sits on the I2C bus of the DSI connector and answers as the touch controller (0x38). With the bus
recovery active (ESPHome's default, at power-up) the STM32 seems to lock / stall the whole bus: SCL stays low from its
first addressing on until the display loses power. `bus_recovery: false` avoids this.

The STM32 emulates only a subset of the touch controller: the touch count (register 0x02) and 4 bytes per point read
in separate transactions, no setup access. `firmware_reads: true` reads it that way.

## Status

- Init: complete, after a cold start, a restart and a flash (OTA) alike
- Touch: complete
- Backlight control: not available. According to our tests the STM32 does not act on any backlight or power
  command of the Raspberry Pi drivers (registers 0x82, 0x83, 0x85, 0x86 at 0x45) nor on registers 0x01-0x07;
  the backlight stays on.
- Checks: upstream's CI jobs run locally (not on GitHub) and an own review; no open findings

## Example

Tested with the Guition JC-ESP32P4-M3-Dev, display on J2.

```yaml
esphome:
  name: esp32-p4-dfr0550-min
  friendly_name: ESP32-P4-DFR0550-Min

esp32:
  variant: ESP32P4
  engineering_sample: true
  flash_size: 16MB
  framework:
    type: esp-idf

external_components:
  - source: github://p1ngb4ck/esphome@dfrobot-dfr0550-upstream
    components: [i2c, ft5x06]
    refresh: always

psram:
  speed: 200MHz

esp_ldo:
  - channel: 3
    voltage: passthrough

esp32_hosted:
  active_high: true
  variant: ESP32C6
  reset_pin: GPIO54
  cmd_pin: GPIO19
  clk_pin: GPIO18
  d0_pin: GPIO14
  d1_pin: GPIO15
  d2_pin: GPIO16
  d3_pin: GPIO17

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret esphome_encryption_key

ota:
  - platform: esphome

logger:
  hardware_uart: UART0

i2c:
  sda: GPIO7
  scl: GPIO8
  bus_recovery: false

display:
  - platform: mipi_dsi
    model: CUSTOM
    dimensions:
      width: 800
      height: 480
    hsync_pulse_width: 2
    hsync_back_porch: 44
    hsync_front_porch: 134
    vsync_pulse_width: 2
    vsync_back_porch: 21
    vsync_front_porch: 7
    pclk_frequency: 30MHz
    lane_bit_rate: 720Mbps
    lanes: 1
    init_sequence:
      - delay 10ms

touchscreen:
  - platform: ft5x06
    address: 0x38
    firmware_reads: true
    transform:
      mirror_x: true
      mirror_y: true
    on_touch:
      - logger.log:
          format: "touch x=%d y=%d"
          args: [touch.x, touch.y]

lvgl:
```

## License

ESPHome's files: [LICENSE](LICENSE). The additions of this branch (the changes to `i2c` and `ft5x06`, this
README and its example): [LICENSE-MIT](LICENSE-MIT).
