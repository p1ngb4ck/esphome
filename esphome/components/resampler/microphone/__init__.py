import esphome.codegen as cg
from esphome.components import audio, microphone
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_CHANNELS,
    CONF_FILTERS,
    CONF_GAIN_FACTOR,
    CONF_ID,
    CONF_MICROPHONE,
    CONF_NAME,
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
    if isinstance(value, dict) and CONF_NAME in value:
        value = dict(value)
        name = cv.All(cv.string_strict, cv.Length(min=1, max=32))(value.pop(CONF_NAME))
    conf = SOURCE_SCHEMA(value)
    if name is not None:
        conf[CONF_NAME] = name
    return conf


def _same_format(config: ConfigType) -> ConfigType:
    # The users set up for one format: every source gives the same bits per sample and channel count
    sources = config[CONF_MICROPHONE]
    for source in sources[1:]:
        if source[CONF_BITS_PER_SAMPLE] != sources[0][CONF_BITS_PER_SAMPLE] or len(source[CONF_CHANNELS]) != len(
            sources[0][CONF_CHANNELS]
        ):
            raise cv.Invalid(
                "every source of a resampler microphone needs the same bits_per_sample and number of channels",
                [CONF_MICROPHONE],
            )
    return config


def _set_stream_limits(config: ConfigType) -> ConfigType:
    # Only the sample rate changes; the bits and channels are those selected from the source microphone
    source = config[CONF_MICROPHONE][0]
    audio.set_stream_limits(
        min_bits_per_sample=source[CONF_BITS_PER_SAMPLE],
        max_bits_per_sample=source[CONF_BITS_PER_SAMPLE],
        min_channels=len(source[CONF_CHANNELS]),
        max_channels=len(source[CONF_CHANNELS]),
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
        index += 1
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)

    cg.add(var.set_target_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_filters(config[CONF_FILTERS]))
    cg.add(var.set_taps(config[CONF_TAPS]))
