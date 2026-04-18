"""OpenDPS Sensor Platform"""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_VOLT,
    UNIT_WATT,
)

from . import OpenDPS

DEPENDENCIES = ["opendps"]

CONF_OPENDPS_ID = "opendps_id"
CONF_VOLTAGE_IN = "voltage_in"
CONF_VOLTAGE_OUT = "voltage_out"
CONF_CURRENT_OUT = "current_out"
CONF_POWER_OUT = "power_out"
CONF_TEMPERATURE_1 = "temperature_1"
CONF_TEMPERATURE_2 = "temperature_2"
CONF_VOLTAGE_SET = "voltage_set"
CONF_CURRENT_SET = "current_set"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_OPENDPS_ID): cv.use_id(OpenDPS),
        cv.Optional(CONF_VOLTAGE_IN): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_OUT): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT_OUT): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_POWER_OUT): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMPERATURE_1): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TEMPERATURE_2): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE_SET): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT_SET): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENDPS_ID])

    if voltage_in_config := config.get(CONF_VOLTAGE_IN):
        sens = await sensor.new_sensor(voltage_in_config)
        cg.add(parent.set_voltage_in_sensor(sens))

    if voltage_out_config := config.get(CONF_VOLTAGE_OUT):
        sens = await sensor.new_sensor(voltage_out_config)
        cg.add(parent.set_voltage_out_sensor(sens))

    if current_out_config := config.get(CONF_CURRENT_OUT):
        sens = await sensor.new_sensor(current_out_config)
        cg.add(parent.set_current_out_sensor(sens))

    if power_out_config := config.get(CONF_POWER_OUT):
        sens = await sensor.new_sensor(power_out_config)
        cg.add(parent.set_power_out_sensor(sens))

    if temp1_config := config.get(CONF_TEMPERATURE_1):
        sens = await sensor.new_sensor(temp1_config)
        cg.add(parent.set_temp1_sensor(sens))

    if temp2_config := config.get(CONF_TEMPERATURE_2):
        sens = await sensor.new_sensor(temp2_config)
        cg.add(parent.set_temp2_sensor(sens))

    if voltage_set_config := config.get(CONF_VOLTAGE_SET):
        sens = await sensor.new_sensor(voltage_set_config)
        cg.add(parent.set_voltage_set_sensor(sens))

    if current_set_config := config.get(CONF_CURRENT_SET):
        sens = await sensor.new_sensor(current_set_config)
        cg.add(parent.set_current_set_sensor(sens))
