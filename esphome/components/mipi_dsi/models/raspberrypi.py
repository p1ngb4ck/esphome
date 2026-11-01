from esphome.components.mipi import delay, flatten_sequence, i2c_write
from esphome.const import CONF_INIT_SEQUENCE, CONF_MIRROR_X, CONF_MIRROR_Y
from esphome.types import ConfigType

from . import DsiDriverChip


# fmt: off
def init_sequence(porta: int) -> list[tuple[int, ...]]:
    return [
        i2c_write(0x45, 0x85, 0x00), delay(30),
        i2c_write(0x45, 0x86, 0x00), delay(8),
        i2c_write(0x45, 0x83, 0x00), delay(8),
        i2c_write(0x45, 0x81, porta), delay(8),
        i2c_write(0x45, 0x82, 0x80), delay(8),
        i2c_write(0x45, 0x83, 0x01), delay(88),
        i2c_write(0x45, 0x83, 0x0D), delay(8),
        i2c_write(0x45, 0x8D, 0x04), delay(8),
        i2c_write(0x45, 0x8C, 0x7C), delay(8),
        i2c_write(0x45, 0x90, 0x00), delay(8),
        i2c_write(0x45, 0x91, 0x00), delay(108),
        i2c_write(0x45, 0x83, 0x0F), delay(8),
        (0x64, 0x04, 0x0F, 0x04, 0x00, 0x00),
        (0x20, 0x04, 0x00, 0x00, 0x00, 0x00),
        (0x10, 0x02, 0x03, 0x00, 0x00, 0x00),
        (0x64, 0x01, 0x05, 0x00, 0x00, 0x00),
        (0x68, 0x01, 0x05, 0x00, 0x00, 0x00),
        (0x44, 0x01, 0x00, 0x00, 0x00, 0x00),
        (0x48, 0x01, 0x00, 0x00, 0x00, 0x00),
        (0x14, 0x01, 0x03, 0x00, 0x00, 0x00),
        (0x24, 0x04, 0x02, 0x00, 0x2C, 0x00),
        (0x28, 0x04, 0x20, 0x03, 0x83, 0x00),
        (0x2C, 0x04, 0x02, 0x00, 0x15, 0x00),
        (0x30, 0x04, 0xE0, 0x01, 0x07, 0x00),
        (0x34, 0x04, 0x01, 0x00, 0x00, 0x00),
        (0x20, 0x04, 0x52, 0x01, 0x10, 0x00),
        delay(100),
        (0x04, 0x01, 0x01, 0x00, 0x00, 0x00),
        (0x04, 0x02, 0x01, 0x00, 0x00, 0x00),
        delay(100),
    ]
# fmt: on


class RpiTouchDisplayV1(DsiDriverChip):
    def get_sequence(
        self, config: ConfigType, add_madctl: bool = True, add_reset: bool = False
    ) -> tuple[int, ...]:
        transform = self.get_transform(config)
        porta = (0x00 if transform[CONF_MIRROR_X] else 0x04) | (
            0x08 if transform[CONF_MIRROR_Y] else 0x00
        )
        return flatten_sequence(
            init_sequence(porta) + list(config.get(CONF_INIT_SEQUENCE, []))
        )


RpiTouchDisplayV1(
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
    eotp=False,
    lp_horizontal_blanking=False,
    lp_vertical_active=False,
    command_mode="generic",
    transforms={CONF_MIRROR_X, CONF_MIRROR_Y},
    initsequence=init_sequence(0x04),
)
