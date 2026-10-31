import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32S31,
    only_on_variant,
)
from esphome.components.camera_encoder import H264Encoder
from esphome.components.usb_host import USB_BLACKLIST_SCHEMA, setup_usb_match, usb_host_ns
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core.entity_helpers import setup_entity

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["usb_host", "camera"]
MULTI_CONF = True

CONF_FORMAT = "format"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_FPS = "fps"
CONF_NUM_URBS = "num_urbs"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_CAMERA = "camera"
CONF_H264_ENCODER = "h264_encoder"

usb_uvc_ns = cg.esphome_ns.namespace("usb_uvc")

UVCClient = usb_uvc_ns.class_(
    "UVCClient",
    usb_host_ns.class_("USBClient"),
    cg.Component,
)

UvcCamera = usb_uvc_ns.class_("UvcCamera", cg.Component)
UvcFormat = usb_uvc_ns.enum("UvcFormat", is_class=True)

UVC_FORMATS = {
    "MJPEG": UvcFormat.MJPEG,
    "UNCOMPRESSED": UvcFormat.UNCOMPRESSED,
    "FRAME_BASED": UvcFormat.FRAME_BASED,
}

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(UVCClient),
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            **USB_BLACKLIST_SCHEMA,
            cv.Optional(CONF_FORMAT, default="MJPEG"): cv.enum(UVC_FORMATS, upper=True),
            cv.Optional(CONF_WIDTH, default=0): cv.uint16_t,
            cv.Optional(CONF_HEIGHT, default=0): cv.uint16_t,
            cv.Optional(CONF_FPS, default=0): cv.uint8_t,
            cv.Optional(CONF_NUM_URBS, default=3): cv.int_range(min=1, max=8),
            # The first plugged camera as a Home Assistant camera entity (its MJPEG frames)
            cv.Optional(CONF_CAMERA): cv.ENTITY_BASE_SCHEMA.extend(
                {
                    cv.GenerateID(): cv.declare_id(UvcCamera),
                    cv.Optional(CONF_H264_ENCODER): cv.use_id(H264Encoder),
                }
            ).extend(cv.COMPONENT_SCHEMA),
        }
    ),
    only_on_variant(
        supported=[VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32P4, VARIANT_ESP32S31]
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_usb_match(var, config)

    if vid := config.get(CONF_VID):
        cg.add(var.set_vid(vid))
    if pid := config.get(CONF_PID):
        cg.add(var.set_pid(pid))

    cg.add(var.set_format(config[CONF_FORMAT]))
    cg.add(var.set_width(config[CONF_WIDTH]))
    cg.add(var.set_height(config[CONF_HEIGHT]))
    cg.add(var.set_fps(config[CONF_FPS]))
    cg.add(var.set_num_urbs(config[CONF_NUM_URBS]))

    cg.add_define("USE_USB_ISOC_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")

    if (camera := config.get(CONF_CAMERA)) is not None:
        cg.add_define("USE_CAMERA")
        cam = cg.new_Pvariable(camera[CONF_ID])
        await setup_entity(cam, camera, "camera")
        await cg.register_component(cam, camera)
        cg.add(cam.set_client(var))
        if (h264 := camera.get(CONF_H264_ENCODER)) is not None:
            cg.add(cam.set_h264_encoder(await cg.get_variable(h264)))
