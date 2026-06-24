import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor
from esphome.components.usb_hid import USBHIDClient

CONF_USB_HID_ID = "usb_hid_id"

# Mouse button binary sensors
CONF_LEFT_BUTTON = "left_button"
CONF_RIGHT_BUTTON = "right_button"
CONF_MIDDLE_BUTTON = "middle_button"

# Mouse axis sensors
CONF_X_DELTA = "x_delta"
CONF_Y_DELTA = "y_delta"
CONF_WHEEL = "wheel"

mouse_button_schema = binary_sensor.binary_sensor_schema().extend(
    {
        cv.GenerateID(CONF_USB_HID_ID): cv.use_id(USBHIDClient),
        cv.Optional(CONF_LEFT_BUTTON): cv.boolean,
        cv.Optional(CONF_RIGHT_BUTTON): cv.boolean,
        cv.Optional(CONF_MIDDLE_BUTTON): cv.boolean,
    }
)

mouse_axis_schema = sensor.sensor_schema().extend(
    {
        cv.GenerateID(CONF_USB_HID_ID): cv.use_id(USBHIDClient),
        cv.Optional(CONF_X_DELTA): cv.boolean,
        cv.Optional(CONF_Y_DELTA): cv.boolean,
        cv.Optional(CONF_WHEEL): cv.boolean,
    }
)


async def new_mouse_button_sensor(config):
    parent = await cg.get_variable(config[CONF_USB_HID_ID])
    var = await binary_sensor.new_binary_sensor(config)
    if config.get(CONF_LEFT_BUTTON):
        cg.add(parent.register_mouse_left_sensor(var))
    elif config.get(CONF_RIGHT_BUTTON):
        cg.add(parent.register_mouse_right_sensor(var))
    elif config.get(CONF_MIDDLE_BUTTON):
        cg.add(parent.register_mouse_middle_sensor(var))
    return var


async def new_mouse_axis_sensor(config):
    parent = await cg.get_variable(config[CONF_USB_HID_ID])
    var = await sensor.new_sensor(config)
    if config.get(CONF_X_DELTA):
        cg.add(parent.register_mouse_x_sensor(var))
    elif config.get(CONF_Y_DELTA):
        cg.add(parent.register_mouse_y_sensor(var))
    elif config.get(CONF_WHEEL):
        cg.add(parent.register_mouse_wheel_sensor(var))
    return var
