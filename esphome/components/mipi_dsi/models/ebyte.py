from . import DsiDriverChip

DsiDriverChip(
    "ECA20-MHA1-I",
    width=1280,
    height=720,
    lanes=2,
    lane_bit_rate="1000Mbps",
    pclk_frequency="74.25MHz",
    hsync_front_porch=110,
    hsync_pulse_width=40,
    hsync_back_porch=220,
    vsync_front_porch=5,
    vsync_pulse_width=5,
    vsync_back_porch=20,
    pixel_mode="24bit",
    color_order="RGB",
    use_axis_flips=False,
    video_mode_only=True,
    continuous_clock=True,
)
