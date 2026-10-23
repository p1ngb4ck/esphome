import pytest

from esphome.components.esp32 import KEY_BOARD, VARIANT_ESP32P4
from esphome.components.mipi import (
    DELAY_FLAG,
    I2C_FLAG,
    DriverChip,
    delay,
    flatten_sequence,
    i2c_write,
)
from esphome.const import CONF_I2C_ID, KEY_VARIANT, PlatformFramework
from tests.component_tests.types import SetCoreConfigCallable

TEST_MODEL = "TEST-I2C-INIT"


def test_i2c_write_entry() -> None:
    assert i2c_write(0x45, 0x85, 0x01) == (I2C_FLAG, 0x45, 0x85, 0x01)
    assert delay(30) == (DELAY_FLAG, 30)


def test_flatten_sequence_with_i2c_writes() -> None:
    sequence = [
        i2c_write(0x45, 0x85, 0x00),
        delay(30),
        (0x10, 0x02, 0x03),
        i2c_write(0x38, 0xA4, 0x01),
    ]
    assert flatten_sequence(sequence) == (
        0x45,
        0xFE,
        0x85,
        0x00,
        30,
        0xFF,
        0x10,
        2,
        0x02,
        0x03,
        0x38,
        0xFE,
        0xA4,
        0x01,
    )


def test_i2c_id_only_for_models_with_i2c_writes(
    set_core_config: SetCoreConfigCallable, monkeypatch: pytest.MonkeyPatch
) -> None:
    set_core_config(
        PlatformFramework.ESP32_IDF,
        platform_data={KEY_BOARD: "esp32-p4-evboard", KEY_VARIANT: VARIANT_ESP32P4},
    )

    from esphome.components.mipi_dsi import display
    from esphome.components.mipi_dsi.models import DsiDriverChip

    monkeypatch.setattr(DriverChip, "models", {})
    model = DsiDriverChip(
        TEST_MODEL,
        width=800,
        height=480,
        lane_bit_rate="720Mbps",
        initsequence=[i2c_write(0x45, 0x85, 0x00), delay(30), (0x10, 0x02, 0x03)],
    )
    monkeypatch.setitem(display.MODELS, TEST_MODEL, model)

    config = display.model_schema({"model": TEST_MODEL})({"model": TEST_MODEL})
    assert CONF_I2C_ID in config

    base = {
        "model": "custom",
        "init_sequence": [[0xA0, 0x01]],
        "lane_bit_rate": "1.5Gbps",
        "dimensions": {"width": 320, "height": 240},
    }
    assert CONF_I2C_ID not in display.CONFIG_SCHEMA({**base, "id": "no_i2c"})
