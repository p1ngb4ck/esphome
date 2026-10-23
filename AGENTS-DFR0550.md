# Agent notes: DFRobot DFR0550 5" DSI display on ESP32-P4

Verified facts only, from the working state of this branch (tested with the Guition JC-ESP32P4-M3-Dev, display on J2).
The tested display is similar to the DFR0550 but has no backlight potentiometer.

## Hardware

- DFR0550 schematic v1.0: an STM32F103C8T6 is on the I2C bus of the DSI connector; the DSI bridge and the touch
  controller are on a separate bus of the STM32.
- On the connector bus the STM32 answers at 0x45 (register 0x80 reads 0xC3, as the Raspberry Pi 7" display's ATTINY88)
  and at 0x38 (touch).
- No DSI init commands needed: the display shows the picture with `model: CUSTOM` and an init sequence of a 10 ms
  delay only.

## Display settings

800x480, H sync 2 / back porch 44 / front porch 134, V 2 / 21 / 7, 30 MHz pixel clock, 720 Mbps, 1 lane, ESP LDO
channel 3 passthrough.

## Bus lock by ESPHome's bus recovery

- ESPHome's I2C bus setup runs a bus recovery (9 SCL pulses and a STOP, the pins driven as GPIO) at power-up.
- After it, the STM32 holds SCL low from its first addressing on until the display loses power; IDF logs
  `I2C transaction timeout detected`. Seen on every cold start with the recovery, also when the first transaction is a
  read. A 13 ms transaction timeout does not change it.
- Without the recovery (`bus_recovery: false`) all transactions work, after a cold start, a restart and an OTA flash.

## Touch

- The STM32 emulates a subset of an FT5x06 at 0x38: TD_STATUS register 0x02 (touch count in the low nibble, 0xFF before
  the first touch, max 5), then per point 4 bytes at 0x03 + 6 x i, each point in its own read.
- Point: event = b0 >> 6, x = (b0 & 0x0F) << 8 | b1, id = b2 >> 4, y = (b2 & 0x0F) << 8 | b3. Only events 1 and 2 are
  touches; untouched slots keep stale data with event 0.
- No setup access (mode, thresholds); `firmware_reads: true` reads only as above.
- Touch coordinates match the picture with `transform` `mirror_x: true` and `mirror_y: true` on the touchscreen.

## Backlight tests (no effect)

All writes to 0x45 were acknowledged without error; the backlight stayed on at full brightness for each:

| Write | Origin |
|---|---|
| `0x86 = 0x00`, `0x86 = 0x10` (PWM) | Linux `rpi-panel-attiny-regulator.c`, `panel-raspberrypi-touchscreen.c` |
| `0x85 = 0x00` (POWERON off) | `panel-raspberrypi-touchscreen.c` |
| `0x86 = 0x00`, then `0x85 = 0x00` | power-off order of `panel-raspberrypi-touchscreen.c` |
| `0x83 = 0x0E` (PORTC, LED_EN off) | `rpi-panel-attiny-regulator.c` |
| `0x83 = 0x0E`, then `0x85 = 0x00` | combination |
| `0x82 = 0x02` (PORTB, LCD_VCC_N, panel supply off) | `rpi-panel-attiny-regulator.c` |
| `0x01` ... `0x07` = 0x00, one write each | registers not used by the Pi drivers |

Readbacks at 0x45 do not reflect the writes: 0x83, 0x85, 0x86 read 0x01, 0x81 and 0x82 read 0x85; a read scan of about 190 of the 256 registers read
0x01 everywhere except the ID 0x80 (0xC3). No backlight control was found.
