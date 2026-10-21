from esphome.components.mipi import delay

from . import DsiDriverChip

# Raspberry Pi 7" Touch Display, first version (800x480): DPI panel behind a Toshiba TC358762XBG DSI-DPI bridge,
# power, resets and backlight through an ATTINY88 at I2C 0x45 (REG_POWERON 0x85, REG_PWM 0x86), touch at 0x38.
# The Raspberry Pi kernel's startup for this display (overlay vc4-kms-dsi-7inch, raspberrypi/linux rpi-6.6.y, read
# 2026-10-21): panel-simple raspberrypi_7inch mode (30 MHz, H 800 / front 131 / sync 2 / back 45, V 480 / 7 / 2 / 22,
# negative HSYNC and VSYNC), tc358762.c bridge init (each register a generic long write of the 16-bit address and the
# 32-bit value, both little-endian; LCDCTRL with the polarity bits of the mode; the LCD timing registers from the
# mode), vc4_dsi.c link (1 lane at pixel clock * 24 = 720 Mbps - that the bridge takes its pixel clock from the byte
# clock is inferred -, RGB888, video mode with sync pulses (non-burst), continuous HS clock).
# The ATTINY must have powered the bridge (REG_POWERON = 1, then REG_PORTB bit 0 set) before this display sets up.
# fmt: off
DsiDriverChip(
    "RPI-7INCH-V1",
    width=800,
    height=480,
    hsync_front_porch=131,
    hsync_pulse_width=2,
    hsync_back_porch=45,
    vsync_front_porch=7,
    vsync_pulse_width=2,
    vsync_back_porch=22,
    pclk_frequency="30MHz",
    lane_bit_rate="720Mbps",
    lanes=1,
    color_depth="24",
    pixel_mode="24bit",
    color_order="RGB",
    video_mode="non_burst_sync_pulses",
    frame_ack=False,
    continuous_clock=True,
    # vc4 sends the EoT packet (no MIPI_DSI_MODE_NO_EOT_PACKET); no LP in the horizontal blanking as the Pi firmware
    # (vc4's setting for it not read)
    lp_horizontal_blanking=False,
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
        (0x24, 0x04, 0x02, 0x00, 0x2D, 0x00),  # LCD_HS_HBP 0x002D0002: HSYNC 2, HBP 45
        (0x28, 0x04, 0x20, 0x03, 0x83, 0x00),  # LCD_HDISP_HFP 0x00830320: 800, HFP 131
        (0x2C, 0x04, 0x02, 0x00, 0x16, 0x00),  # LCD_VS_VBP 0x00160002: VSYNC 2, VBP 22
        (0x30, 0x04, 0xE0, 0x01, 0x07, 0x00),  # LCD_VDISP_VFP 0x000701E0: 480, VFP 7
        delay(100),
        (0x04, 0x01, 0x01, 0x00, 0x00, 0x00),  # PPI_STARTPPI
        (0x04, 0x02, 0x01, 0x00, 0x00, 0x00),  # DSI_STARTDSI
        delay(100),
    ],
)
