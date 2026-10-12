"""Bluetooth Classic host plumbing shared by bt_hid and a2dp_source.

Bluedroid has a single Classic GAP callback; this component owns it and hands every event
to each registered handler. Auto-loaded, no configuration.
"""

import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add_define("USE_BT_CLASSIC")
