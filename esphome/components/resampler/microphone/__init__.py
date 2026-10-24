import esphome.codegen as cg
from esphome.components import audio, esp32, microphone
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_CHANNELS,
    CONF_FILTERS,
    CONF_GAIN_FACTOR,
    CONF_ID,
    CONF_MICROPHONE,
    CONF_MODE,
    CONF_NAME,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
    PLATFORM_ESP32,
)
from esphome.types import ConfigType

from .. import CONF_TAPS, resampler_ns, validate_taps

AUTO_LOAD = ["audio"]
DEPENDENCIES = ["microphone"]

ResamplerMicrophone = resampler_ns.class_(
    "ResamplerMicrophone", cg.Component, microphone.Microphone
)

CONF_USB_AUDIO = "usb_audio"
CONF_ECHO_CANCELLATION = "echo_cancellation"
CONF_REFERENCE_CHANNEL = "reference_channel"
CONF_FILTER_LENGTH = "filter_length"

AEC_MODES = {
    "sr_low_cost": 0,
    "sr_high_perf": 1,
    "voip_low_cost": 3,
    "voip_high_perf": 4,
    "fd_low_cost": 5,
    "fd_high_perf": 6,
}

AEC_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MODE, default="voip_low_cost"): cv.enum(AEC_MODES, lower=True),
        cv.Optional(CONF_REFERENCE_CHANNEL, default=1): cv.int_range(0, 1),
        cv.Optional(CONF_FILTER_LENGTH, default=4): cv.int_range(1, 8),
    }
)


def _validate_aec_variant(value):
    if esp32.get_esp32_variant() not in (esp32.VARIANT_ESP32S3, esp32.VARIANT_ESP32P4):
        raise cv.Invalid("echo_cancellation needs an ESP32-S3 or ESP32-P4 (esp-sr)")
    return value

SOURCE_SCHEMA = microphone.microphone_source_schema(
    min_bits_per_sample=16,
    max_bits_per_sample=32,
    min_channels=1,
    max_channels=2,
)

GROUP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_USB_AUDIO): cv.use_id(cg.Component),
        cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.int_range(16, 32),
        cv.Optional(CONF_CHANNELS, default="0"): cv.All(
            cv.ensure_list(cv.int_range(0, 7)), cv.Length(min=1, max=2)
        ),
        cv.Optional(CONF_GAIN_FACTOR, default="1"): cv.int_range(1, 64),
    }
)


def _is_group(source) -> bool:
    return CONF_USB_AUDIO in source


def _source_entry(value):
    if isinstance(value, dict) and CONF_USB_AUDIO in value:
        return GROUP_SCHEMA(value)
    name = None
    aec = None
    if isinstance(value, dict) and CONF_NAME in value:
        value = dict(value)
        name = cv.All(cv.string_strict, cv.Length(min=1, max=32))(value.pop(CONF_NAME))
    if isinstance(value, dict) and CONF_ECHO_CANCELLATION in value:
        value = dict(value)
        aec = cv.All(AEC_SCHEMA, _validate_aec_variant)(value.pop(CONF_ECHO_CANCELLATION) or {})
    conf = SOURCE_SCHEMA(value)
    if name is not None:
        conf[CONF_NAME] = name
    if aec is not None:
        if len(conf[CONF_CHANNELS]) != 2 or conf[CONF_BITS_PER_SAMPLE] != 16:
            raise cv.Invalid(
                "echo_cancellation needs a 16 bit source with 2 channels: the microphone and the reference"
            )
        conf[CONF_ECHO_CANCELLATION] = aec
    return conf


def _same_format(config: ConfigType) -> ConfigType:
    # The users set up for one format: every source gives the same bits per sample and channel count
    sources = config[CONF_MICROPHONE]
    converts_channels = CONF_NUM_CHANNELS in config
    for source in sources[1:]:
        if source[CONF_BITS_PER_SAMPLE] != sources[0][CONF_BITS_PER_SAMPLE] or (
            not converts_channels and len(source[CONF_CHANNELS]) != len(sources[0][CONF_CHANNELS])
        ):
            raise cv.Invalid(
                "every source of a resampler microphone needs the same bits_per_sample and, without num_channels, "
                "the same number of channels",
                [CONF_MICROPHONE],
            )
    return config


def _set_stream_limits(config: ConfigType) -> ConfigType:
    # Only the sample rate changes; the bits and channels are those selected from the source microphone
    source = config[CONF_MICROPHONE][0]
    audio.set_stream_limits(
        min_bits_per_sample=source[CONF_BITS_PER_SAMPLE],
        max_bits_per_sample=source[CONF_BITS_PER_SAMPLE],
        min_channels=config.get(CONF_NUM_CHANNELS, len(source[CONF_CHANNELS])),
        max_channels=config.get(CONF_NUM_CHANNELS, len(source[CONF_CHANNELS])),
        min_sample_rate=config[CONF_SAMPLE_RATE],
        max_sample_rate=config[CONF_SAMPLE_RATE],
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(ResamplerMicrophone),
            # One source, or a list: the first passed on at boot, the others by set_source() at runtime
            cv.Required(CONF_MICROPHONE): cv.All(
                cv.ensure_list(_source_entry),
                cv.Length(min=1),
            ),
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(8000, 48000),
            cv.Optional(CONF_NUM_CHANNELS): cv.int_range(1, 2),
            cv.Optional(CONF_FILTERS, default=16): cv.int_range(min=2, max=1024),
            cv.Optional(CONF_TAPS, default=16): validate_taps,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    _same_format,
    _set_stream_limits,
)


_FINAL_SOURCE = microphone.final_validate_microphone_source_schema("resampler")


def _final_validate(config: ConfigType) -> ConfigType:
    for source in config[CONF_MICROPHONE]:
        if not _is_group(source):
            _FINAL_SOURCE(source)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    index = 0
    groups = 0
    for source in config[CONF_MICROPHONE]:
        if _is_group(source):
            mask = sum(1 << channel for channel in source[CONF_CHANNELS])
            cg.add(var.add_source_group(source[CONF_BITS_PER_SAMPLE], source[CONF_GAIN_FACTOR], mask))
            hub = await cg.get_variable(source[CONF_USB_AUDIO])
            cg.add(
                hub.add_microphone_route(
                    cg.RawExpression(
                        "[](esphome::microphone::Microphone *mic, const char *name, uint32_t key, bool added) { "
                        f"if (added) {{ {var}->add_group_source({groups}, mic, name, key); }} "
                        f"else {{ {var}->remove_source(mic); }} }}"
                    )
                )
            )
            groups += 1
            continue
        cg.add(var.add_source(await microphone.microphone_source_to_code(source)))
        cg.add(var.set_source_name(index, source.get(CONF_NAME, str(source[CONF_MICROPHONE]))))
        if aec := source.get(CONF_ECHO_CANCELLATION):
            cg.add_define("USE_RESAMPLER_MICROPHONE_AEC")
            esp32.add_idf_component(name="espressif/esp-dsp", ref="^1.8.0")
            esp32.add_idf_component(name="espressif/esp-sr", ref="^2.5.3")
            cg.add_cxx_build_flag("-includecstdint")
            cg.add(
                var.set_source_echo_cancellation(
                    index, aec[CONF_MODE], aec[CONF_REFERENCE_CHANNEL], aec[CONF_FILTER_LENGTH]
                )
            )
        index += 1
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)

    cg.add(var.set_target_sample_rate(config[CONF_SAMPLE_RATE]))
    if CONF_NUM_CHANNELS in config:
        cg.add(var.set_num_channels(config[CONF_NUM_CHANNELS]))
    cg.add(var.set_filters(config[CONF_FILTERS]))
    cg.add(var.set_taps(config[CONF_TAPS]))
