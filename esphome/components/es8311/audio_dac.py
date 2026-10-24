import esphome.codegen as cg
from esphome.components import i2c
from esphome.components.audio_dac import AudioDac
import esphome.config_validation as cv
from esphome.const import CONF_BITS_PER_SAMPLE, CONF_MIC_GAIN, CONF_SAMPLE_RATE
from esphome.types import ConfigType

CODEOWNERS = ["@kroimon", "@kahrendt"]
DEPENDENCIES = ["i2c"]

es8311_ns = cg.esphome_ns.namespace("es8311")
ES8311 = es8311_ns.class_("ES8311", AudioDac, cg.Component, i2c.I2CDevice)

CONF_USE_MCLK = "use_mclk"
CONF_USE_MICROPHONE = "use_microphone"
CONF_DAC_REFERENCE = "dac_reference"
CONF_VOLUME_MIN_DB = "volume_min_db"
CONF_VOLUME_MAX_DB = "volume_max_db"

es8311_resolution = es8311_ns.enum("ES8311Resolution")
ES8311_BITS_PER_SAMPLE_ENUM = {
    16: es8311_resolution.ES8311_RESOLUTION_16,
    24: es8311_resolution.ES8311_RESOLUTION_24,
    32: es8311_resolution.ES8311_RESOLUTION_32,
}

es8311_mic_gain = es8311_ns.enum("ES8311MicGain")
ES8311_MIC_GAIN_ENUM = es8311_mic_gain.options(
    "0DB",
    "6DB",
    "12DB",
    "18DB",
    "24DB",
    "30DB",
    "36DB",
    "42DB",
    prefix="ES8311_MIC_GAIN_",
)


_validate_bits = cv.float_with_unit("bits", "bit")

def _validate_volume_range(config):
    if config[CONF_VOLUME_MIN_DB] >= config[CONF_VOLUME_MAX_DB]:
        raise cv.Invalid(f"{CONF_VOLUME_MIN_DB} must be below {CONF_VOLUME_MAX_DB}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ES8311),
            cv.Optional(CONF_BITS_PER_SAMPLE, default="16bit"): cv.All(
                _validate_bits, cv.enum(ES8311_BITS_PER_SAMPLE_ENUM)
            ),
            cv.Optional(CONF_MIC_GAIN, default="42DB"): cv.enum(
                ES8311_MIC_GAIN_ENUM, upper=True
            ),
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=1),
            cv.Optional(CONF_USE_MCLK, default=True): cv.boolean,
            cv.Optional(CONF_USE_MICROPHONE, default=False): cv.boolean,
            cv.Optional(CONF_DAC_REFERENCE, default=False): cv.boolean,
            # The DAC volume range the volume 0..1 spans, in dB (the register: -95.5 .. +32 dB in
            # 0.5 dB steps). Above 0 dB the DAC adds digital gain: a full scale signal clips.
            cv.Optional(CONF_VOLUME_MIN_DB, default=-95.5): cv.float_range(min=-95.5, max=32.0),
            cv.Optional(CONF_VOLUME_MAX_DB, default=32.0): cv.float_range(min=-95.5, max=32.0),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x18)),
    _validate_volume_range,
)


async def to_code(config: ConfigType) -> None:
    var = await i2c.new_i2c_component(config)

    cg.add(var.set_bits_per_sample(config[CONF_BITS_PER_SAMPLE]))
    cg.add(var.set_mic_gain(config[CONF_MIC_GAIN]))
    cg.add(var.set_sample_frequency(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_use_mclk(config[CONF_USE_MCLK]))
    cg.add(var.set_use_mic(config[CONF_USE_MICROPHONE]))
    if config[CONF_DAC_REFERENCE]:
        cg.add(var.set_dac_reference(True))
    if (config[CONF_VOLUME_MIN_DB], config[CONF_VOLUME_MAX_DB]) != (-95.5, 32.0):
        cg.add(var.set_volume_range_db(config[CONF_VOLUME_MIN_DB], config[CONF_VOLUME_MAX_DB]))
