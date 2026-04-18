"""OpenDPS Binary Sensor Platform"""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_CONNECTIVITY, DEVICE_CLASS_RUNNING

from . import OpenDPS

DEPENDENCIES = ["opendps"]

CONF_OPENDPS_ID = "opendps_id"
CONF_OUTPUT_ENABLED = "output_enabled"
CONF_CONNECTED = "connected"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENDPS_ID): cv.use_id(OpenDPS),
        cv.Optional(CONF_OUTPUT_ENABLED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_RUNNING
        ),
        cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENDPS_ID])

    if output_enabled_config := config.get(CONF_OUTPUT_ENABLED):
        sens = await binary_sensor.new_binary_sensor(output_enabled_config)
        cg.add(parent.set_output_enabled_binary_sensor(sens))

    if connected_config := config.get(CONF_CONNECTED):
        sens = await binary_sensor.new_binary_sensor(connected_config)
        cg.add(parent.set_connected_binary_sensor(sens))
