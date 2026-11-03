import esphome.codegen as cg
from esphome.components import display, esp32
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32S31,
    get_esp32_variant,
    only_on_variant,
)
from esphome.components.lvgl.defines import CONF_LVGL_ID
from esphome.components.lvgl.video_output import (
    VIDEO_OUTPUT_SCHEMA,
    final_validate_video_output,
    video_output_to_code,
)
from esphome.components.camera import CONF_ENCODER, ENCODER_SCHEMA, encoder_to_code
from esphome.components.usb_host import USB_BLACKLIST_SCHEMA, setup_usb_match, usb_host_ns
import esphome.config_validation as cv
from esphome.const import CONF_DISPLAY_ID, CONF_ID
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

def _validate_canvas(config):
    if CONF_LVGL_ID in config and get_esp32_variant() != VARIANT_ESP32P4:
        raise cv.Invalid("Showing frames in LVGL needs the ESP32-P4's JPEG decoder")
    return config


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
            cv.Optional(CONF_DISPLAY_ID): cv.use_id(display.Display),
            # The first plugged camera as a Home Assistant camera entity (its MJPEG frames)
            cv.Optional(CONF_CAMERA): cv.ENTITY_BASE_SCHEMA.extend(
                {
                    cv.GenerateID(): cv.declare_id(UvcCamera),
                    cv.Optional(CONF_ENCODER, default={}): ENCODER_SCHEMA,
                }
            ).extend(cv.COMPONENT_SCHEMA),
        }
    ).extend(VIDEO_OUTPUT_SCHEMA),
    only_on_variant(
        supported=[VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32P4, VARIANT_ESP32S31]
    ),
    _validate_canvas,
)

FINAL_VALIDATE_SCHEMA = final_validate_video_output


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
    if CONF_DISPLAY_ID in config:
        cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    if CONF_LVGL_ID in config:
        cg.add_define("USE_UVC_VIDEO_OUTPUT")
        esp32.require_hw_jpeg()
        await video_output_to_code(var, config)

    cg.add_define("USE_USB_ISOC_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")

    if (camera := config.get(CONF_CAMERA)) is not None:
        cg.add_define("USE_CAMERA")
        cam = cg.new_Pvariable(camera[CONF_ID])
        await setup_entity(cam, camera, "camera")
        await cg.register_component(cam, camera)
        cg.add(cam.set_client(var))
        if (encoder := await encoder_to_code(camera[CONF_ENCODER])) is not None:
            cg.add(cam.set_h264_encoder(encoder))
