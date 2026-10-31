import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_KEY

from .. import CONF_HID_ID, HIDInput, hid_ns
from ..keys import validate_key

HIDKeyBinarySensor = hid_ns.class_("HIDKeyBinarySensor", binary_sensor.BinarySensor)

# On while a key or button is held.
CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(HIDKeyBinarySensor).extend(
    {
        cv.GenerateID(CONF_HID_ID): cv.use_id(HIDInput),
        cv.Required(CONF_KEY): validate_key,
    }
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_HID_ID])
    cg.add(var.attach(parent, *config[CONF_KEY]))
