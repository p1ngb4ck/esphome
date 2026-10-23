# Agent notes: Raspberry Pi 7" Touch Display v1 on ESP32-P4

Verified facts only, from the working state of this branch (tested with the Guition JC-ESP32P4-M3-Dev).

## Hardware

- DSI-DPI bridge Toshiba TC358762, 1 DSI lane, panel 800x480 RGB888.
- ATTINY88 at I2C 0x45 (register 0x80 reads 0xC3): panel power, resets, scan direction, backlight PWM.
- Touch controller at I2C 0x38, FT5x06 family, vendor ID 0x79; TD_STATUS reads 0xFF until the first touch.
- ATTINY88 and touch controller are on the I2C bus of the DSI connector.

## ATTINY88 registers used

| Register | Use |
|---|---|
| 0x80 | ID, reads 0xC3 |
| 0x81 PORTA | 0x04 LCD_LR, 0x08 LCD_UD (scan direction) |
| 0x82 PORTB | 0x80 LCD_MAIN |
| 0x83 PORTC | bit 0 LED_EN, bit 1 RST_TP_N, bit 2 RST_LCD_N, bit 3 RST_BRIDGE_N |
| 0x85 | POWERON |
| 0x86 | backlight PWM, 0-255 |
| 0x8C / 0x8D, 0x90 / 0x91 | bridge register address / data, written through the ATTINY88 |

## Start-up (the model's init sequence, sent after `esp_lcd_panel_init()`)

1. ATTINY88, `i2c_write` to 0x45 as in Linux `rpi-panel-attiny-regulator.c`, delay after each:
   `0x85=0x00` 30 ms, `0x86=0x00` 8 ms, `0x83=0x00` 8 ms, `0x81=PORTA` 8 ms, `0x82=0x80` 8 ms, `0x83=0x01` 88 ms,
   `0x83=0x0D` 8 ms, `0x8D=0x04` 8 ms, `0x8C=0x7C` 8 ms, `0x90=0x00` 8 ms, `0x91=0x00` 108 ms (bridge register
   0x047C = 0), `0x83=0x0F` 8 ms.
2. TC358762, DSI generic long writes (16-bit register, then 32-bit value, little-endian):

| Register | Value | Meaning |
|---|---|---|
| 0x0464 SYSCTRL | 0x0000040F | PCLKDIV /3: pixel clock = DSI byte clock / 3 |
| 0x0420 LCDCTRL | 0x00000000 | off during setup |
| 0x0210 | 0x00000003 | lane enable |
| 0x0164 / 0x0168 | 0x00000005 | PPI D0S / D1S CLRSIPOCOUNT |
| 0x0144 / 0x0148 | 0x00000000 | PPI D0S / D1S ATMR |
| 0x0114 | 0x00000003 | PPI LPTXTIMECNT |
| 0x0424 | 0x002C0002 | H sync 2, back porch 44 |
| 0x0428 | 0x00830320 | H active 800, front porch 131 |
| 0x042C | 0x00150002 | V sync 2, back porch 21 |
| 0x0430 | 0x000701E0 | V active 480, front porch 7 |
| 0x0434 VFUEN | 0x00000001 | load the timing |
| 0x0420 LCDCTRL | 0x00100152 | VTGEN (bit 1), RGB888 (bits 6:4 = 5), DPI_EN (bit 8), DCLK_POL (bit 20) |
| (100 ms) | | |
| 0x0104 | 0x00000001 | STARTPPI |
| 0x0204 | 0x00000001 | STARTDSI |
| (100 ms) | | |

LCDCTRL bit meanings per Tomi Valkeinen's TC358762 fix series (v4); the defines in older Linux `tc358762.c` are wrong.
The picture comes up after a cold start, a restart and an OTA flash.

## DSI link

- 30 MHz pixel clock, 720 Mbps (pixel clock x 24), 1 lane, RGB888, burst video mode (IDF default).
- Seen with other values: 31.25 MHz flickers; 26.667 MHz at 720 Mbps flickers.
- Timing H 2 / 44 / 131, V 2 / 21 / 7 (sync / back / front), the same on DSI and in the bridge's timing generator.
- VTGEN off: single lines shift by a pixel (teeth on vertical edges). VTGEN on: correct picture.
- Generic writes required; the same bytes as DCS long writes fail.
- Acknowledge requests and frame acknowledge off: with them the bus turnaround hangs (the TC358762 does not answer).
- Continuous HS clock on, EoT packet off, LP in horizontal blanking and active lines off: tested as one group, with the
  upstream defaults the picture breaks.

## Touch, backlight, orientation

- Touch: `ft5x06` at 0x38 with vendor ID 0x79 accepted and a touch count above 5 treated as no touch.
- Backlight: ATTINY88 register 0x86, 0-255.
- Orientation: PORTA 0x04 normal; `transform` `mirror_x` clears 0x04, `mirror_y` sets 0x08 (mirrored by the panel's
  scan direction). The touch controller is not affected: the touchscreen needs the same `transform`.
