from esphome.components.mipi import delay

from . import DsiDriverChip

# Raspberry Pi 7" Touch Display, first version (800x480): DPI panel behind a Toshiba TC358762XBG DSI-DPI bridge,
# power, resets and backlight through an ATTINY88 at I2C 0x45 (REG_POWERON 0x85, REG_PWM 0x86), touch at 0x38.
# Bridge init after Tomi Valkeinen's tc358762 fix series (dri-devel / lkml, "drm/bridge: tc358762: Various small
# fixes" v4, 2026-09; read 2026-10-21), which corrects the LCDCTRL fields of the Linux driver (VTGEN is bit 1, the
# old 0x150 is PXLFORM RGB888 + DPI_EN) and always enables the video timing generator: with VTG off the bridge copies
# the DSI timing and hsync occasionally lengthens by one clock. SYSCTRL (PCLKDIV /3: pixel clock = DSI byte clock / 3,
# so lane bit rate = pixel clock x 24) first, LCDCTRL cleared before the setup (DPI_EN resets set), timing registers,
# VFUEN, then LCDCTRL with VTGEN; each register a generic long write of the 16-bit address and the 32-bit value,
# both little-endian. Polarities from panel-simple powertip,ph800480t013-idf02 (the panel in this display): HSYNC /
# VSYNC active low (the *_POL bits make them active high), DE high, data sampled on the falling edge (DCLK_POL).
# Timing: sync + back porch 46 horizontally and 23 vertically (the series' panel fix: 24 shifts the image; the Pi
# firmware runs H 2 / 44, V 2 / 21), the same on DSI and in the bridge; 30 MHz = 240 MHz / 8 on the P4.
# Link as vc4_dsi.c (raspberrypi/linux rpi-6.6.y): 1 lane, RGB888, video mode with sync pulses (non-burst),
# continuous HS clock, no EoT packet, LP only once per frame.
# The ATTINY powers the panel and releases the bridge reset (Linux rpi-panel-attiny-regulator.c sequence) from the
# display's setup, once the DSI lanes are in LP-11 (Linux: tc358762 pre_enable_prev_first, vc4_dsi drives STOP first).
# fmt: off
DsiDriverChip(
    "RPI-7INCH-V1",
    width=800,
    height=480,
    hsync_front_porch=131,
    hsync_pulse_width=2,
    hsync_back_porch=44,
    vsync_front_porch=7,
    vsync_pulse_width=2,
    vsync_back_porch=21,
    pclk_frequency="30MHz",
    lane_bit_rate="720Mbps",
    lanes=1,
    color_depth="24",
    pixel_mode="24bit",
    color_order="RGB",
    video_mode="non_burst_sync_pulses",
    frame_ack=False,
    continuous_clock=True,
    # vc4_dsi.c: DSI_CTRL_HSDT_EOT_DISABLE always (no EoT packet), DISP0 LP_STOP_PERFRAME (LP once per frame, none
    # in the horizontal blanking or the active lines)
    eotp=False,
    lp_horizontal_blanking=False,
    lp_vertical_active=False,
    command_mode="generic",
    no_transform=True,
    i2c_address=0x45,
    # ATTINY88 registers
    i2c_init_sequence=[
        (0x85, 0x00), delay(30),  # POWERON off
        (0x86, 0x00), delay(8),   # PWM: backlight off
        (0x83, 0x00), delay(8),   # PORTC: bridge, LCD and touch in reset
        (0x81, 0x04), delay(8),   # PORTA: LCD_LR (0x08 LCD_UD: rotated 180 degrees)
        (0x82, 0x80), delay(8),   # PORTB: LCD_MAIN
        (0x83, 0x01), delay(88),  # PORTC: LED_EN
        (0x83, 0x0D), delay(8),   # PORTC: RST_BRIDGE_N, RST_LCD_N released
        (0x8D, 0x04), delay(8),   # bridge register 0x047C = 0
        (0x8C, 0x7C), delay(8),
        (0x90, 0x00), delay(8),
        (0x91, 0x00), delay(108),
        (0x83, 0x0F), delay(8),   # PORTC: RST_TP_N released
    ],
    # No read_registers, no ack, no frame ack: the TC358762 does not answer a bus turnaround - lane 0 stays reversed
    # (DSI host PHY_STATUS phy_direction 1, gen_rd_cmd_busy, IDF runs without timeouts) and nothing is sent after
    # it, not even video (seen 2026-10-21). Linux never reads it.
    initsequence=[
        (0x64, 0x04, 0x0F, 0x04, 0x00, 0x00),  # SYSCTRL 0x040F: DPI data / strobe drive 4 mA, PCLKDIV /3
        (0x20, 0x04, 0x00, 0x00, 0x00, 0x00),  # LCDCTRL 0: DPI off during the setup
        (0x10, 0x02, 0x03, 0x00, 0x00, 0x00),  # DSI_LANEENABLE: clock lane + data lane 0
        (0x64, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D0S_CLRSIPOCOUNT
        (0x68, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D1S_CLRSIPOCOUNT
        (0x44, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D0S_ATMR
        (0x48, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D1S_ATMR
        (0x14, 0x01, 0x03, 0x00, 0x00, 0x00),  # PPI_LPTXTIMECNT
        # Video timing generator: sync | back porch << 16, active | front porch << 16
        (0x24, 0x04, 0x02, 0x00, 0x2C, 0x00),  # LCDC_HSR_HBPR 0x002C0002: HSYNC 2, HBP 44
        (0x28, 0x04, 0x20, 0x03, 0x83, 0x00),  # LCDC_HDISPR_HFPR 0x00830320: 800, HFP 131
        (0x2C, 0x04, 0x02, 0x00, 0x15, 0x00),  # LCDC_VSR_VBPR 0x00150002: VSYNC 2, VBP 21
        (0x30, 0x04, 0xE0, 0x01, 0x07, 0x00),  # LCDC_VDISPR_VFPR 0x000701E0: 480, VFP 7
        (0x34, 0x04, 0x01, 0x00, 0x00, 0x00),  # LCDC_VFUEN: update the timing
        (0x20, 0x04, 0x52, 0x01, 0x10, 0x00),  # LCDCTRL 0x00100152: VTGEN, PXLFORM RGB888, DPI_EN, DCLK_POL
        delay(100),
        (0x04, 0x01, 0x01, 0x00, 0x00, 0x00),  # PPI_STARTPPI
        (0x04, 0x02, 0x01, 0x00, 0x00, 0x00),  # DSI_STARTDSI
        delay(100),
    ],
)
