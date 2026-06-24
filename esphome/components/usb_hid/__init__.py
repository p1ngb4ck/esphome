import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, text_sensor
from esphome.components.usb_host import USBClient
from esphome.const import CONF_ID

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]
AUTO_LOAD = ["usb_host"]

SUPPORTED_VARIANTS = [
    esp32.const.VARIANT_ESP32S2,
    esp32.const.VARIANT_ESP32S3,
    esp32.const.VARIANT_ESP32P4,
]

CONF_USB_HID_ID = "usb_hid_id"
CONF_KEYBOARD = "keyboard"
CONF_MOUSE = "mouse"
CONF_GAMEPAD = "gamepad"
CONF_LAYOUT = "layout"
CONF_DEVICE_NAME = "device_name"

usb_hid_ns = cg.esphome_ns.namespace("usb_hid")
USBHIDClient = usb_hid_ns.class_("USBHIDClient", USBClient, cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBHIDClient),
            cv.Optional(CONF_KEYBOARD): cv.Schema(
                {
                    cv.Optional(CONF_LAYOUT, default="us"): cv.one_of(
                        "us", "uk", "de", "fr", "es", lower=True
                    ),
                }
            ),
            cv.Optional(CONF_MOUSE): cv.Schema({}),
            cv.Optional(CONF_GAMEPAD): cv.Schema({}),
            cv.Optional(CONF_DEVICE_NAME): text_sensor.text_sensor_schema(),
        }
    ),
    esp32.only_on_variant(supported=SUPPORTED_VARIANTS),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_KEYBOARD in config:
        cg.add_build_flag("-DUSB_HID_ENABLE_KEYBOARD")
        layout = config[CONF_KEYBOARD].get(CONF_LAYOUT, "us")
        layout_map = {
            "us": "KEYBOARD_LAYOUT_US",
            "uk": "KEYBOARD_LAYOUT_UK",
            "de": "KEYBOARD_LAYOUT_DE",
            "fr": "KEYBOARD_LAYOUT_FR",
            "es": "KEYBOARD_LAYOUT_ES",
        }
        cg.add_define(layout_map[layout])

    if CONF_MOUSE in config:
        cg.add_build_flag("-DUSB_HID_ENABLE_MOUSE")

    if CONF_GAMEPAD in config:
        cg.add_build_flag("-DUSB_HID_ENABLE_GAMEPAD")

    if (device_name_cfg := config.get(CONF_DEVICE_NAME)) is not None:
        sens = await text_sensor.new_text_sensor(device_name_cfg)
        cg.add(var.register_device_name_sensor(sens))

    # Interrupt transfers share the bulk transfer path in ESP-IDF
    cg.add_define("USE_USB_BULK_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")
