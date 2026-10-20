from esphome.components.mipi import delay

from . import DsiDriverChip

# Raspberry Pi 7" Touch Display, first version (800x480): DPI panel behind a Toshiba TC358762XBG DSI-DPI bridge,
# power, resets and backlight through an ATTINY88 at I2C 0x45 (REG_POWERON 0x85, REG_PWM 0x86), touch at 0x38.
# Timing, DSI mode and bridge init from Linux drivers/gpu/drm/panel/panel-raspberrypi-touchscreen.c and
# drivers/gpu/drm/bridge/tc358762.c: 1 lane, RGB888, video mode with sync pulses (non-burst), each bridge register
# written as a generic long write of the 16-bit address and the 32-bit value, both little-endian.
# The ATTINY must have powered the bridge (REG_POWERON = 1, then REG_PORTB bit 0 set) before this display sets up.
# fmt: off
DsiDriverChip(
    "RPI-7INCH-V1",
    width=800,
    height=480,
    hsync_front_porch=1,
    hsync_pulse_width=2,
    hsync_back_porch=46,
    vsync_front_porch=7,
    vsync_pulse_width=2,
    vsync_back_porch=21,
    pclk_frequency="26MHz",
    lane_bit_rate="650Mbps",
    lanes=1,
    color_depth="24",
    pixel_mode="24bit",
    color_order="RGB",
    video_mode="non_burst_sync_pulses",
    frame_ack=False,
    command_mode="generic",
    no_transform=True,
    initsequence=[
        (0x10, 0x02, 0x03, 0x00, 0x00, 0x00),  # DSI_LANEENABLE: clock lane + data lane 0
        (0x64, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D0S_CLRSIPOCOUNT
        (0x68, 0x01, 0x05, 0x00, 0x00, 0x00),  # PPI_D1S_CLRSIPOCOUNT
        (0x44, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D0S_ATMR
        (0x48, 0x01, 0x00, 0x00, 0x00, 0x00),  # PPI_D1S_ATMR
        (0x14, 0x01, 0x03, 0x00, 0x00, 0x00),  # PPI_LPTXTIMECNT
        (0x50, 0x04, 0x00, 0x00, 0x00, 0x00),  # SPICMR
        (0x20, 0x04, 0x50, 0x01, 0x10, 0x00),  # LCDCTRL 0x00100150: VSYNC delay 1, RGB888, VTGEN
        (0x64, 0x04, 0x0F, 0x04, 0x00, 0x00),  # SYSCTRL 0x040F
        delay(100),
        (0x04, 0x01, 0x01, 0x00, 0x00, 0x00),  # PPI_STARTPPI
        (0x04, 0x02, 0x01, 0x00, 0x00, 0x00),  # DSI_STARTDSI
        delay(100),
    ],
)
