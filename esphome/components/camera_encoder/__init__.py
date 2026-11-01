import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import add_idf_component
from esphome.components.esp32.const import VARIANT_ESP32P4
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_ID, CONF_TYPE
from esphome.types import ConfigType

CODEOWNERS = ["@DT-art1"]
DOMAIN = "camera_encoder"

AUTO_LOAD = ["camera"]

CONF_BUFFER_EXPAND_SIZE = "buffer_expand_size"
CONF_ENCODER_BUFFER_ID = "encoder_buffer_id"
CONF_QUALITY = "quality"
CONF_BITRATE = "bitrate"
CONF_GOP = "gop"
CONF_FPS = "fps"
CONF_QP_MIN = "qp_min"
CONF_QP_MAX = "qp_max"

ESP32_CAMERA_ENCODER = "esp32_camera"
H264_ENCODER = "h264"

camera_ns = cg.esphome_ns.namespace("camera")
camera_encoder_ns = cg.esphome_ns.namespace("camera_encoder")

Encoder = camera_ns.class_("Encoder")
EncoderBufferImpl = camera_encoder_ns.class_("EncoderBufferImpl")

ESP32CameraJPEGEncoder = camera_encoder_ns.class_("ESP32CameraJPEGEncoder", Encoder)
H264Encoder = camera_encoder_ns.class_("H264Encoder", Encoder)

MAX_JPEG_BUFFER_SIZE_2MB = 2 * 1024 * 1024

ESP32_CAMERA_ENCODER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ESP32CameraJPEGEncoder),
        cv.Optional(CONF_QUALITY, default=80): cv.int_range(1, 100),
        cv.Optional(CONF_BUFFER_SIZE, default=4096): cv.int_range(
            1024, MAX_JPEG_BUFFER_SIZE_2MB
        ),
        cv.Optional(CONF_BUFFER_EXPAND_SIZE, default=1024): cv.int_range(
            0, MAX_JPEG_BUFFER_SIZE_2MB
        ),
        cv.GenerateID(CONF_ENCODER_BUFFER_ID): cv.declare_id(EncoderBufferImpl),
    }
)

H264_ENCODER_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(H264Encoder),
            cv.Optional(CONF_BITRATE, default=2000000): cv.int_range(100000, 20000000),
            cv.Optional(CONF_GOP, default=30): cv.int_range(1, 255),
            cv.Optional(CONF_FPS, default=30): cv.int_range(1, 60),
            cv.Optional(CONF_QP_MIN, default=25): cv.int_range(0, 51),
            cv.Optional(CONF_QP_MAX, default=40): cv.int_range(0, 51),
        }
    ),
    esp32.only_on_variant(supported=[VARIANT_ESP32P4]),
)


def _validate_qp(config: ConfigType) -> ConfigType:
    if config[CONF_TYPE] == H264_ENCODER and config[CONF_QP_MIN] > config[CONF_QP_MAX]:
        raise cv.Invalid(f"{CONF_QP_MIN} must not be larger than {CONF_QP_MAX}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            ESP32_CAMERA_ENCODER: ESP32_CAMERA_ENCODER_SCHEMA,
            H264_ENCODER: H264_ENCODER_SCHEMA,
        },
        default_type=ESP32_CAMERA_ENCODER,
    ),
    _validate_qp,
)


async def to_code(config: ConfigType) -> None:
    if config[CONF_TYPE] == H264_ENCODER:
        add_idf_component(name="espressif/esp_h264", ref="1.4.1")
        cg.add_define("USE_CAMERA_H264_ENCODER")
        cg.new_Pvariable(
            config[CONF_ID],
            config[CONF_BITRATE],
            config[CONF_GOP],
            config[CONF_FPS],
            config[CONF_QP_MIN],
            config[CONF_QP_MAX],
        )
        return
    buffer = cg.new_Pvariable(config[CONF_ENCODER_BUFFER_ID])
    cg.add(buffer.set_buffer_size(config[CONF_BUFFER_SIZE]))
    if config[CONF_TYPE] == ESP32_CAMERA_ENCODER:
        add_idf_component(name="espressif/esp32-camera", ref="2.1.7")
        cg.add_define("USE_ESP32_CAMERA_JPEG_ENCODER")
        var = cg.new_Pvariable(
            config[CONF_ID],
            config[CONF_QUALITY],
            buffer,
        )
        cg.add(var.set_buffer_expand_size(config[CONF_BUFFER_EXPAND_SIZE]))
