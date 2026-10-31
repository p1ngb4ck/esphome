import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_TYPE

from .. import CONF_HID_ID, HIDInput, hid_ns

HIDTextSensor = hid_ns.class_("HIDTextSensor", text_sensor.TextSensor)
HIDTextSensorType = hid_ns.enum("HIDTextSensorType")

TYPES = {
    # The name of the last key or button pressed.
    "last_key": HIDTextSensorType.HID_TEXT_LAST_KEY,
    # What the last key press typed.
    "text": HIDTextSensorType.HID_TEXT_TYPED,
    # A line of typed text, published on Enter (barcode scanners end their codes so).
    "line": HIDTextSensorType.HID_TEXT_LINE,
}

CONFIG_SCHEMA = text_sensor.text_sensor_schema(HIDTextSensor).extend(
    {
        cv.GenerateID(CONF_HID_ID): cv.use_id(HIDInput),
        cv.Required(CONF_TYPE): cv.enum(TYPES, lower=True),
    }
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    parent = await cg.get_variable(config[CONF_HID_ID])
    cg.add(var.attach(parent, config[CONF_TYPE]))
