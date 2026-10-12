from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

from . import (
    BLE_CLIENT_HID_SCHEMA,
    register_battery_sensor,
    register_last_event_value_sensor,
)

DEPENDENCIES = ["ble_client_hid"]

TYPE_BATTERY = "battery"
TYPE_LAST_EVENT_VALUE = "last_event_value"

CONFIG_SCHEMA = cv.typed_schema(
    {
        TYPE_BATTERY: sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ).extend(BLE_CLIENT_HID_SCHEMA),
        # The value of the last decoded control, as in the esphome.hid_events event.
        TYPE_LAST_EVENT_VALUE: sensor.sensor_schema(
            accuracy_decimals=0,
        ).extend(BLE_CLIENT_HID_SCHEMA),
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    if config[CONF_TYPE] == TYPE_BATTERY:
        await register_battery_sensor(var, config)
    else:
        await register_last_event_value_sensor(var, config)
