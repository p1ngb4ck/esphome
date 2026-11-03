from collections.abc import Callable
from pathlib import Path

from esphome.components.esp32 import KEY_BOARD, VARIANT_ESP32P4
from esphome.const import KEY_VARIANT, PlatformFramework
from tests.component_tests.types import SetCoreConfigCallable

BASE = {
    "model": "custom",
    "init_sequence": [[0xA0, 0x01]],
    "lane_bit_rate": "1.5Gbps",
    "dimensions": {"width": 320, "height": 240},
}

DEFAULTS = {
    "frame_ack": True,
    "continuous_clock": False,
    "eotp": True,
    "lp_horizontal_blanking": True,
    "lp_vertical_active": True,
}

OVERRIDES = {
    "frame_ack": False,
    "continuous_clock": True,
    "eotp": False,
    "lp_horizontal_blanking": False,
    "lp_vertical_active": False,
}


def test_link_option_defaults(set_core_config: SetCoreConfigCallable) -> None:
    set_core_config(
        PlatformFramework.ESP32_IDF,
        platform_data={KEY_BOARD: "esp32-p4-evboard", KEY_VARIANT: VARIANT_ESP32P4},
    )

    from esphome.components.mipi_dsi.display import CONFIG_SCHEMA

    config = CONFIG_SCHEMA({**BASE, "id": "link_defaults"})
    assert {key: config[key] for key in DEFAULTS} == DEFAULTS


def test_link_option_overrides(set_core_config: SetCoreConfigCallable) -> None:
    set_core_config(
        PlatformFramework.ESP32_IDF,
        platform_data={KEY_BOARD: "esp32-p4-evboard", KEY_VARIANT: VARIANT_ESP32P4},
    )

    from esphome.components.mipi_dsi.display import CONFIG_SCHEMA

    config = CONFIG_SCHEMA({**BASE, "id": "link_overrides", **OVERRIDES})
    assert {key: config[key] for key in OVERRIDES} == OVERRIDES


def test_link_option_code_generation(
    generate_main: Callable[[str | Path], str],
    component_fixture_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_fixture_path("mipi_dsi_link_options.yaml"))
    for display_id, values in (
        ("link_defaults", DEFAULTS),
        ("link_overrides", OVERRIDES),
    ):
        for key, value in values.items():
            assert f"{display_id}->set_{key}({str(value).lower()});" in main_cpp
