from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_TYPE

from . import (
    BLE_CLIENT_HID_SCHEMA,
    register_last_event_code_text_sensor,
    register_last_event_usage_text_sensor,
)

DEPENDENCIES = ["ble_client_hid"]

# The usage name ("Volume Increment") and the page_usage code ("12_233") of the last decoded
# control, as in the esphome.hid_events event. For key names and typed text see the hid
# text_sensor platform.
TYPE_LAST_EVENT_USAGE = "last_event_usage"
TYPE_LAST_EVENT_CODE = "last_event_code"

CONFIG_SCHEMA = cv.typed_schema(
    {
        TYPE_LAST_EVENT_USAGE: text_sensor.text_sensor_schema().extend(
            BLE_CLIENT_HID_SCHEMA
        ),
        TYPE_LAST_EVENT_CODE: text_sensor.text_sensor_schema().extend(
            BLE_CLIENT_HID_SCHEMA
        ),
    }
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    if config[CONF_TYPE] == TYPE_LAST_EVENT_USAGE:
        await register_last_event_usage_text_sensor(var, config)
    else:
        await register_last_event_code_text_sensor(var, config)
