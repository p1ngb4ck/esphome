import esphome.codegen as cg
from esphome.components.esp32 import only_on_variant, VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3
from esphome.components.usb_host import usb_host_ns
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]

CONF_FORMAT = "format"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_FPS = "fps"
CONF_NUM_URBS = "num_urbs"
CONF_VID = "vid"
CONF_PID = "pid"

usb_uvc_ns = cg.esphome_ns.namespace("usb_uvc")

UVCClient = usb_uvc_ns.class_(
    "UVCClient",
    usb_host_ns.class_("USBClient"),
    cg.Component,
)

UvcFormat = usb_uvc_ns.enum("UvcFormat")

UVC_FORMATS = {
    "MJPEG": UvcFormat.MJPEG,
    "UNCOMPRESSED": UvcFormat.UNCOMPRESSED,
}

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(UVCClient),
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_FORMAT, default="MJPEG"): cv.enum(UVC_FORMATS, upper=True),
            cv.Optional(CONF_WIDTH, default=0): cv.uint16_t,
            cv.Optional(CONF_HEIGHT, default=0): cv.uint16_t,
            cv.Optional(CONF_FPS, default=0): cv.uint8_t,
            cv.Optional(CONF_NUM_URBS, default=3): cv.int_range(min=1, max=8),
        }
    ),
    only_on_variant(supported=[VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32P4]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if (vid := config.get(CONF_VID)):
        cg.add(var.set_vid(vid))
    if (pid := config.get(CONF_PID)):
        cg.add(var.set_pid(pid))

    cg.add(var.set_format(config[CONF_FORMAT]))
    cg.add(var.set_width(config[CONF_WIDTH]))
    cg.add(var.set_height(config[CONF_HEIGHT]))
    cg.add(var.set_fps(config[CONF_FPS]))
    cg.add(var.set_num_urbs(config[CONF_NUM_URBS]))

    cg.add_define("USE_USB_ISOC_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")
