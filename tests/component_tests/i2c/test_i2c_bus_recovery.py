from collections.abc import Callable
from pathlib import Path

import pytest

from esphome import config_validation as cv
from esphome.const import PlatformFramework
from tests.component_tests.types import SetCoreConfigCallable


def test_bus_recovery_accepted(set_core_config: SetCoreConfigCallable) -> None:
    set_core_config(PlatformFramework.ESP8266_ARDUINO)

    from esphome.components.i2c import CONFIG_SCHEMA

    config = CONFIG_SCHEMA({"sda": 4, "scl": 5, "bus_recovery": False})
    assert config["bus_recovery"] is False
    assert "bus_recovery" not in CONFIG_SCHEMA({"sda": 4, "scl": 5})


def test_bus_recovery_rejected_on_host(set_core_config: SetCoreConfigCallable) -> None:
    set_core_config(PlatformFramework.HOST_NATIVE)

    from esphome.components.i2c import CONFIG_SCHEMA

    with pytest.raises(cv.Invalid, match="only available with framework"):
        CONFIG_SCHEMA({"bus_recovery": False})


def test_bus_recovery_code_generation(
    generate_main: Callable[[str | Path], str],
    component_fixture_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_fixture_path("i2c_bus_recovery.yaml"))
    assert "bus_no_recovery->set_bus_recovery(false);" in main_cpp
    assert "bus_default->set_bus_recovery(" not in main_cpp
