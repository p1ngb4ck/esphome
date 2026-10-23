from collections.abc import Callable
from pathlib import Path
import re

import pytest

from esphome import config_validation as cv
from esphome.components.esp32 import KEY_BOARD, VARIANT_ESP32P4
from esphome.const import KEY_VARIANT, PlatformFramework
from tests.component_tests.types import SetCoreConfigCallable

BASE = {
    "model": "custom",
    "init_sequence": [[0xA0, 0x01]],
    "lane_bit_rate": "1.5Gbps",
    "dimensions": {"width": 320, "height": 240},
}


def init_sequence(main_cpp: str, display_id: str) -> str:
    name = re.search(rf"{display_id}->set_init_sequence\((\w+), \d+\);", main_cpp)
    assert name is not None
    values = re.search(
        rf"static constexpr uint8_t {name.group(1)}\[\] PROGMEM = \{{([^}}]*)\}};",
        main_cpp,
    )
    assert values is not None
    return values.group(1)


def test_command_mode_validation(set_core_config: SetCoreConfigCallable) -> None:
    set_core_config(
        PlatformFramework.ESP32_IDF,
        platform_data={KEY_BOARD: "esp32-p4-evboard", KEY_VARIANT: VARIANT_ESP32P4},
    )

    from esphome.components.mipi_dsi.display import CONFIG_SCHEMA

    assert CONFIG_SCHEMA({**BASE, "id": "dcs_default"})["command_mode"] == "dcs"
    assert (
        CONFIG_SCHEMA({**BASE, "id": "generic_upper", "command_mode": "GENERIC"})[
            "command_mode"
        ]
        == "generic"
    )
    with pytest.raises(cv.Invalid, match="Unknown value 'bta'"):
        CONFIG_SCHEMA({**BASE, "id": "invalid_mode", "command_mode": "bta"})


def test_generic_commands_code_generation(
    generate_main: Callable[[str | Path], str],
    component_fixture_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_fixture_path("mipi_dsi_generic_commands.yaml"))
    assert "generic_display->set_generic_commands(true);" in main_cpp
    assert "dcs_display->set_generic_commands(false);" in main_cpp
    assert init_sequence(main_cpp, "generic_display") == "100, 5, 4, 15, 4, 0, 0, 10, 255, 17, 0"
    dcs = init_sequence(main_cpp, "dcs_display")
    assert dcs.startswith("100, 5, 4, 15, 4, 0, 0, ")
    assert dcs.endswith("17, 0, 41, 0")
