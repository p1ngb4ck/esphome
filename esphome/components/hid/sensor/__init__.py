import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv

from .. import CONF_HID_ID, HIDInput, hid_ns
from ..keys import validate_key

CONF_CONTROL = "control"

HIDControlSensor = hid_ns.class_("HIDControlSensor", sensor.Sensor)

# One control as a sensor: sticks -100..100, triggers, sliders and pedals 0..100 (the
# normalized values of on_control), any other control as its raw value.
CONFIG_SCHEMA = sensor.sensor_schema(HIDControlSensor, accuracy_decimals=0).extend(
    {
        cv.GenerateID(CONF_HID_ID): cv.use_id(HIDInput),
        cv.Required(CONF_CONTROL): validate_key,
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    parent = await cg.get_variable(config[CONF_HID_ID])
    cg.add(var.attach(parent, *config[CONF_CONTROL]))
