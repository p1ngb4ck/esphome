import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, text_sensor
from esphome.components.usb_hid import USBHIDClient

CONF_USB_HID_ID = "usb_hid_id"
CONF_KEYCODE = "keycode"

# Per-key binary sensor: fires when a specific HID keycode is held
keyboard_key_schema = binary_sensor.binary_sensor_schema().extend(
    {
        cv.GenerateID(CONF_USB_HID_ID): cv.use_id(USBHIDClient),
        cv.Required(CONF_KEYCODE): cv.hex_uint8_t,
    }
)

# Keyboard text sensor: publishes the name of each pressed key
keyboard_text_schema = text_sensor.text_sensor_schema().extend(
    {
        cv.GenerateID(CONF_USB_HID_ID): cv.use_id(USBHIDClient),
    }
)


async def new_keyboard_key_sensor(config):
    parent = await cg.get_variable(config[CONF_USB_HID_ID])
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(parent.register_keyboard_key_sensor(var, config[CONF_KEYCODE]))
    return var


async def new_keyboard_text_sensor(config):
    parent = await cg.get_variable(config[CONF_USB_HID_ID])
    var = await text_sensor.new_text_sensor(config)
    cg.add(parent.register_keyboard_sensor(var))
    return var
