"""Bluetooth Classic host plumbing shared by bt_hid and a2dp_source.

Bluedroid has a single Classic GAP callback; this component owns it and hands every event
to each registered handler. Auto-loaded, no configuration.
"""

import esphome.codegen as cg
from esphome.components.esp32 import add_idf_sdkconfig_option, get_esp32_variant
from esphome.components.esp32.const import VARIANT_ESP32S31
import esphome.config_validation as cv
from esphome.core import CORE

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]

CONFIG_SCHEMA = cv.Schema({})


def _dongle_host(full_config) -> bool:
    return any(c.get("bluedroid") for c in full_config.get("usb_bluetooth") or [])


async def to_code(config):
    cg.add_define("USE_BT_CLASSIC")
    if get_esp32_variant() == VARIANT_ESP32S31 and not _dongle_host(CORE.config):
        add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
        add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", True)
        add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
        add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", False)
