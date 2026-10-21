from esphome.components.mipi import delay

from . import DsiDriverChip

# Raspberry Pi 7" Touch Display, first version (800x480): DPI panel behind a Toshiba TC358762XBG DSI-DPI bridge,
# power, resets and backlight through an ATTINY88 at I2C 0x45 (REG_POWERON 0x85, REG_PWM 0x86), touch at 0x38.
# The Raspberry Pi kernel's startup for this display (overlay vc4-kms-dsi-7inch, raspberrypi/linux rpi-6.6.y, read
# 2026-10-21): panel-simple raspberrypi_7inch mode (30 MHz, H 800 / front 131 / sync 2 / back 45, V 480 / 7 / 2 / 22,
# negative HSYNC and VSYNC), tc358762.c bridge init (each register a generic long write of the 16-bit address and the
# 32-bit value, both little-endian; LCDCTRL with the polarity bits of the mode; the LCD timing registers from the
# mode), vc4_dsi.c link (1 lane, RGB888, video mode with sync pulses (non-burst), continuous HS clock, no EoT packet,
# LP only once per frame).
# Timing: the mode the Pi 4's firmware runs (pixel valve 1 read on a working Pi, 2026-10-21): H sync 2 / back 44 /
# front 61, V 2 / 21 / 7, pixel clock 27.777 MHz = 750 Mbps / 27. Here 26.667 MHz (240 MHz / 9, the DPI clock is an
# integer divider of PLL_F240M) and 720 Mbps keep the Pi's 27 link bits per pixel; the bridge's LCD timing registers
# get the same mode.
# The ATTINY must have powered the panel and released the bridge reset (Linux rpi-panel-attiny-regulator.c sequence)
# before this display sets up.
# fmt: off
DsiDriverChip(
    "RPI-7INCH-V1",
    width=800,
    height=480,
    hsync_front_porch=61,
    hsync_pulse_width=2,
    hsync_back_porch=44,
    vsync_front_porch=7,
    vsync_pulse_width=2,
    vsync_back_porch=21,
    pclk_frequency="26.6667MHz",
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
    # No read_registers, no ack, no frame ack: the TC358762 does not answer a bus turnaround - lane 0 stays reversed
    # (DSI host PHY_STATUS phy_direction 1, gen_rd_cmd_busy, IDF runs without timeouts) and nothing is sent after
    # it, not even video (seen 2026-10-21). Linux never reads it.
    initsequence=[
        (0x10, 0x02, 0x03, 0x00, 0x00, 0x00),  # DSI_LANEENABLE: clock lane + data lane 0
        (0x64, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D0S_CLRSIPOCOUNT
        (0x68, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D1S_CLRSIPOCOUNT
        (0x44, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D0S_ATMR
        (0x48, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D1S_ATMR
        (0x14, 0x01, 0x03, 0x00, 0x00, 0x00),  # PPI_LPTXTIMECNT
        (0x50, 0x04, 0x00, 0x00, 0x00, 0x00),  # SPICMR
        (0x20, 0x04, 0x50, 0x01, 0x1A, 0x00),  # LCDCTRL 0x001A0150: VSYNC delay 1, RGB888, VTGEN, HSYNC + VSYNC negative
        (0x64, 0x04, 0x0F, 0x04, 0x00, 0x00),  # SYSCTRL 0x040F
        # LCD timing for the bridge's own timing generator (VTGEN) - raspberrypi/linux rpi-6.6.y tc358762.c writes
        # these four, mainline does not: sync | back porch << 16, active | front porch << 16
        (0x24, 0x04, 0x02, 0x00, 0x2C, 0x00),  # LCD_HS_HBP 0x002C0002: HSYNC 2, HBP 44
        (0x28, 0x04, 0x20, 0x03, 0x3D, 0x00),  # LCD_HDISP_HFP 0x003D0320: 800, HFP 61
        (0x2C, 0x04, 0x02, 0x00, 0x15, 0x00),  # LCD_VS_VBP 0x00150002: VSYNC 2, VBP 21
        (0x30, 0x04, 0xE0, 0x01, 0x07, 0x00),  # LCD_VDISP_VFP 0x000701E0: 480, VFP 7
        delay(100),
        (0x04, 0x01, 0x01, 0x00, 0x00, 0x00),  # PPI_STARTPPI
        (0x04, 0x02, 0x01, 0x00, 0x00, 0x00),  # DSI_STARTDSI
        delay(100),
    ],
)
