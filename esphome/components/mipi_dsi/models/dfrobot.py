from esphome.components.mipi import delay, i2c_write

from . import DsiDriverChip

# fmt: off
DsiDriverChip(
    "DFROBOT-DFR0550",
    width=800,
    height=480,
    hsync_front_porch=134,
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
        i2c_write(0x45, 0x85, 0x00), delay(30),
        i2c_write(0x45, 0x86, 0x00),
        i2c_write(0x45, 0x83, 0x00), delay(8),
        i2c_write(0x45, 0x81, 0x04), delay(8),
        i2c_write(0x45, 0x82, 0x80), delay(8),
        i2c_write(0x45, 0x83, 0x01), delay(80),
        i2c_write(0x45, 0x83, 0x0D), delay(8),
        i2c_write(0x45, 0x8D, 0x04), delay(8),
        i2c_write(0x45, 0x8C, 0x7C), delay(8),
        i2c_write(0x45, 0x90, 0x00), delay(8),
        i2c_write(0x45, 0x91, 0x00), delay(100),
        i2c_write(0x45, 0x83, 0x0F),
        i2c_write(0x45, 0x86, 0xFF, 10),
    ],
)
