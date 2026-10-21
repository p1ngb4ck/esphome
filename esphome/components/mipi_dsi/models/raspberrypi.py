from esphome.components.mipi import delay

from . import DsiDriverChip

# Raspberry Pi 7" Touch Display v1: TC358762 DSI-DPI bridge; an ATTINY88 (I2C 0x45) must power the panel and
# release the bridge reset first. Bridge pixel clock = DSI byte clock / 3, so lane_bit_rate = pclk x 24.
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
    frame_ack=False,
    continuous_clock=True,
    eotp=False,
    lp_horizontal_blanking=False,
    lp_vertical_active=False,
    command_mode="generic",
    no_transform=True,
    initsequence=[
        (0x64, 0x04, 0x0F, 0x04, 0x00, 0x00),  # SYSCTRL: PCLKDIV /3
        (0x20, 0x04, 0x00, 0x00, 0x00, 0x00),  # LCDCTRL: DPI off
        (0x10, 0x02, 0x03, 0x00, 0x00, 0x00),  # DSI_LANEENABLE
        (0x64, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D0S_CLRSIPOCOUNT
        (0x68, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D1S_CLRSIPOCOUNT
        (0x44, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D0S_ATMR
        (0x48, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D1S_ATMR
        (0x14, 0x01, 0x03, 0x00, 0x00, 0x00),  # PPI_LPTXTIMECNT
        # Timing generator: sync | back porch << 16, active | front porch << 16
        (0x24, 0x04, 0x02, 0x00, 0x2C, 0x00),  # LCDC_HSR_HBPR
        (0x28, 0x04, 0x20, 0x03, 0x83, 0x00),  # LCDC_HDISPR_HFPR
        (0x2C, 0x04, 0x02, 0x00, 0x15, 0x00),  # LCDC_VSR_VBPR
        (0x30, 0x04, 0xE0, 0x01, 0x07, 0x00),  # LCDC_VDISPR_VFPR
        (0x34, 0x04, 0x01, 0x00, 0x00, 0x00),  # LCDC_VFUEN
        (0x20, 0x04, 0x52, 0x01, 0x10, 0x00),  # LCDCTRL: VTGEN, RGB888, DPI_EN, DCLK_POL
        delay(100),
        (0x04, 0x01, 0x01, 0x00, 0x00, 0x00),  # PPI_STARTPPI
        (0x04, 0x02, 0x01, 0x00, 0x00, 0x00),  # DSI_STARTDSI
        delay(100),
    ],
)
