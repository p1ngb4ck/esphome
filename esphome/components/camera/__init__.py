import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32.const import VARIANT_ESP32P4
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE
from esphome.types import ConfigType

CODEOWNERS = ["@DT-art1", "@bdraco"]
DOMAIN = "camera"

CONF_ENCODER = "encoder"
CONF_BITRATE = "bitrate"
CONF_GOP = "gop"
CONF_FPS = "fps"
CONF_QP_MIN = "qp_min"
CONF_QP_MAX = "qp_max"
ENCODER_JPEG = "jpeg"
ENCODER_H264 = "h264"

camera_ns = cg.esphome_ns.namespace("camera")
Encoder = camera_ns.class_("Encoder")
H264Encoder = camera_ns.class_("H264Encoder", Encoder)


def _validate_qp(config: ConfigType) -> ConfigType:
    if config[CONF_TYPE] == ENCODER_H264 and config[CONF_QP_MIN] > config[CONF_QP_MAX]:
        raise cv.Invalid(f"{CONF_QP_MIN} must not be larger than {CONF_QP_MAX}")
    return config


ENCODER_SCHEMA = cv.All(
    cv.typed_schema(
        {
            ENCODER_JPEG: cv.Schema({}),
            ENCODER_H264: cv.All(
                cv.Schema(
                    {
                        cv.GenerateID(): cv.declare_id(H264Encoder),
                        cv.Optional(CONF_BITRATE, default=2000000): cv.int_range(
                            100000, 20000000
                        ),
                        cv.Optional(CONF_GOP, default=30): cv.int_range(1, 255),
                        cv.Optional(CONF_FPS, default=30): cv.int_range(1, 60),
                        cv.Optional(CONF_QP_MIN, default=25): cv.int_range(0, 51),
                        cv.Optional(CONF_QP_MAX, default=40): cv.int_range(0, 51),
                    }
                ),
                esp32.only_on_variant(supported=[VARIANT_ESP32P4]),
            ),
        },
        default_type=ENCODER_JPEG,
        lower=True,
    ),
    _validate_qp,
)


async def encoder_to_code(config: ConfigType):
    if config[CONF_TYPE] != ENCODER_H264:
        return None
    esp32.require_h264_encoder()
    cg.add_define("USE_CAMERA_H264_ENCODER")
    return cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_BITRATE],
        config[CONF_GOP],
        config[CONF_FPS],
        config[CONF_QP_MIN],
        config[CONF_QP_MAX],
    )
