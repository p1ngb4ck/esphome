import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.usb_host import usb_host_ns
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]
AUTO_LOAD = ["audio"]
MULTI_CONF = False

usb_audio_ns = cg.esphome_ns.namespace("usb_audio")

USBAudioClient = usb_audio_ns.class_(
    "USBAudioClient",
    usb_host_ns.class_("USBClient"),
    cg.Component,
)

CONF_USB_AUDIO_ID = "usb_audio_id"
CONF_MICROPHONE_BUFFER_SIZE = "microphone_buffer_size"
CONF_SPEAKER_BUFFER_SIZE = "speaker_buffer_size"
CONF_DEFAULT_BUFFER_SIZE = 6400

SUPPORTED_VARIANTS = [
    esp32.const.VARIANT_ESP32S2,
    esp32.const.VARIANT_ESP32S3,
    esp32.const.VARIANT_ESP32P4,
]

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBAudioClient),
            cv.Optional(
                CONF_MICROPHONE_BUFFER_SIZE, default=CONF_DEFAULT_BUFFER_SIZE
            ): cv.positive_int,
            cv.Optional(
                CONF_SPEAKER_BUFFER_SIZE, default=CONF_DEFAULT_BUFFER_SIZE
            ): cv.positive_int,
        }
    ),
    esp32.only_on_variant(supported=SUPPORTED_VARIANTS),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_microphone_buffer_size(config[CONF_MICROPHONE_BUFFER_SIZE]))
    cg.add(var.set_speaker_buffer_size(config[CONF_SPEAKER_BUFFER_SIZE]))

    cg.add_define("USE_USB_ISOC_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")
