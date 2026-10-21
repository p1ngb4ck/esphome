from esphome.components.mipi import delay

from . import DsiDriverChip

# DFRobot DFR0550 5" DSI display V1.0 (800x480, 60 Hz): an STM32F103 on the connector's I2C sets up the DSI-RGB
# bridge itself, so nothing is sent over DSI (one delay keeps the sequence non-empty). The Pi drives it as its 7" v1
# (vc4-kms-dsi-7inch: 1 lane, RGB888, no EoT, LP once per frame); H 2/44 and V 2/21/7 as the Pi firmware's mode,
# front porch for 60 Hz at 30 MHz.
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
        delay(10),
    ],
)
