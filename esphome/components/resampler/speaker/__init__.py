import esphome.codegen as cg
from esphome.components import audio, psram, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BUFFER_DURATION,
    CONF_CHANNEL,
    CONF_FILTERS,
    CONF_ID,
    CONF_NAME,
    CONF_NUM_CHANNELS,
    CONF_OUTPUT_SPEAKER,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
    CONF_TASK_STACK_IN_PSRAM,
    PLATFORM_ESP32,
)
from esphome.core.entity_helpers import inherit_property_from
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash
from esphome.types import ConfigType

from .. import CONF_TAPS, resampler_ns, validate_taps

AUTO_LOAD = ["audio"]
CODEOWNERS = ["@kahrendt"]

ResamplerSpeaker = resampler_ns.class_(
    "ResamplerSpeaker", cg.Component, speaker.Speaker
)

PASSTHROUGH = "passthrough"

CONF_SOURCE_SPEAKERS = "source_speakers"
CONF_USB_AUDIO = "usb_audio"
FORMAT_KEYS = (CONF_SAMPLE_RATE, CONF_BITS_PER_SAMPLE, CONF_NUM_CHANNELS)
_CHANNEL_COUNTS = {"mono": 1, "left": 1, "right": 1, "stereo": 2}
_LIMIT_KEYS = {
    CONF_SAMPLE_RATE: ("min_sample_rate", "max_sample_rate"),
    CONF_BITS_PER_SAMPLE: ("min_bits_per_sample", "max_bits_per_sample"),
    CONF_NUM_CHANNELS: ("min_channels", "max_channels"),
}


def _number(value):
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def _own_format(conf) -> dict:
    fmt = {}
    for key in FORMAT_KEYS:
        if (value := _number(conf.get(key))) is not None:
            fmt[key] = value
    if CONF_NUM_CHANNELS not in fmt and conf.get(CONF_CHANNEL) in _CHANNEL_COUNTS:
        fmt[CONF_NUM_CHANNELS] = _CHANNEL_COUNTS[conf[CONF_CHANNEL]]
    for key, (low, high) in _LIMIT_KEYS.items():
        value = _number(conf.get(low))
        if key not in fmt and value is not None and value == _number(conf.get(high)):
            fmt[key] = value
    return fmt


def resolve_speaker_format(fconf, speaker_id, known: dict | None = None) -> dict:
    fmt = dict(known or {})
    seen = set()
    while speaker_id is not None and str(speaker_id) not in seen:
        if all(key in fmt for key in FORMAT_KEYS):
            break
        seen.add(str(speaker_id))
        path = fconf.get_path_for_id(speaker_id)[:-1]
        confs = [fconf.get_config_for_path(path)]
        if len(path) >= 2 and path[-2] == CONF_SOURCE_SPEAKERS:
            confs.append(fconf.get_config_for_path(path[:-2]))
        speaker_id = None
        for conf in confs:
            for key, value in _own_format(conf).items():
                fmt.setdefault(key, value)
            if speaker_id is None:
                speaker_id = conf.get(CONF_OUTPUT_SPEAKER)
    return fmt


def _unique_names(outputs):
    names = [o[CONF_NAME] for o in outputs if CONF_NAME in o]
    if len(names) != len(set(names)):
        raise cv.Invalid("output names must be unique")
    return outputs


def _is_group(output) -> bool:
    return CONF_USB_AUDIO in output


def usb_audio_format(fconf, hub_id, domain) -> dict | None:
    for block in fconf.get(CONF_USB_AUDIO) or []:
        if str(block.get(CONF_ID)) == str(hub_id):
            if domain in block:
                return {key: int(block[domain][key]) for key in FORMAT_KEYS}
            return {CONF_SAMPLE_RATE: 48000, CONF_BITS_PER_SAMPLE: 16, CONF_NUM_CHANNELS: 2}
    return None


OUTPUT_SCHEMA = cv.Any(
    cv.Schema(
        {
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=32)),
            cv.Optional(CONF_SAMPLE_RATE): cv.int_range(min=8000, max=192000),
            cv.Optional(CONF_BITS_PER_SAMPLE): cv.one_of(16, 24, 32, int=True),
            cv.Optional(CONF_NUM_CHANNELS): cv.int_range(min=1, max=8),
        }
    ),
    cv.Schema({cv.Required(CONF_USB_AUDIO): cv.use_id(cg.Component)}),
)


def _is_output_list(config: ConfigType) -> bool:
    return isinstance(config[CONF_OUTPUT_SPEAKER], list)


def _set_stream_limits(config: ConfigType) -> ConfigType:
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=32,
    )(config)

    return config


def output_formats(fconf, conf) -> list[dict]:
    formats = []
    for output in conf[CONF_OUTPUT_SPEAKER]:
        if _is_group(output):
            fmt = usb_audio_format(fconf, output[CONF_USB_AUDIO], CONF_SPEAKER)
        else:
            known = {key: output[key] for key in FORMAT_KEYS if key in output}
            fmt = resolve_speaker_format(fconf, output[CONF_SPEAKER], known)
        formats.append(fmt or {})
    return formats


def _validate_output_list(config: ConfigType) -> None:
    fconf = fv.full_config.get()
    outputs = config[CONF_OUTPUT_SPEAKER]
    for output in outputs:
        if _is_group(output):
            fmt = usb_audio_format(fconf, output[CONF_USB_AUDIO], CONF_SPEAKER)
            if fmt is None:
                raise cv.Invalid(
                    f"resampler output usb_audio '{output[CONF_USB_AUDIO]}': no such usb_audio block"
                )
            if fmt[CONF_BITS_PER_SAMPLE] not in (16, 24, 32):
                raise cv.Invalid(
                    f"resampler output usb_audio '{output[CONF_USB_AUDIO]}': {fmt[CONF_BITS_PER_SAMPLE]} bits per sample "
                    "is not supported (16, 24 or 32)"
                )
            output.update(fmt)
            continue
        known = {key: output[key] for key in FORMAT_KEYS if key in output}
        fmt = resolve_speaker_format(fconf, output[CONF_SPEAKER], known)
        if missing := [key for key in FORMAT_KEYS if key not in fmt]:
            raise cv.Invalid(
                f"resampler output '{output[CONF_NAME]}': its speaker does not fix "
                f"{', '.join(missing)} -- set {', '.join(missing)} on the output"
            )
        if fmt[CONF_BITS_PER_SAMPLE] not in (16, 24, 32):
            raise cv.Invalid(
                f"resampler output '{output[CONF_NAME]}': {fmt[CONF_BITS_PER_SAMPLE]} bits per sample "
                "is not supported (16, 24 or 32)"
            )
        output.update(fmt)
    for key in (CONF_SAMPLE_RATE, CONF_NUM_CHANNELS):
        if key not in config:
            config[key] = max(output[key] for output in outputs)


def _validate_audio_compatibility(config: ConfigType) -> None:
    if _is_output_list(config):
        _validate_output_list(config)
        return
    inherit_property_from(CONF_NUM_CHANNELS, CONF_OUTPUT_SPEAKER)(config)
    inherit_property_from(CONF_SAMPLE_RATE, CONF_OUTPUT_SPEAKER)(config)

    # In passthrough mode the output bits per sample is determined at runtime from the input stream, so there is
    # nothing to inherit or validate against the output speaker.
    passthrough = config.get(CONF_BITS_PER_SAMPLE) == PASSTHROUGH
    if not passthrough:
        inherit_property_from(CONF_BITS_PER_SAMPLE, CONF_OUTPUT_SPEAKER)(config)

    audio.final_validate_audio_schema(
        "source_speaker",
        audio_device=CONF_OUTPUT_SPEAKER,
        bits_per_sample=cv.UNDEFINED
        if passthrough
        else config.get(CONF_BITS_PER_SAMPLE),
        channels=config.get(CONF_NUM_CHANNELS),
        sample_rate=config.get(CONF_SAMPLE_RATE),
    )(config)


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(ResamplerSpeaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.Any(
                cv.use_id(speaker.Speaker),
                cv.All(cv.ensure_list(OUTPUT_SCHEMA), cv.Length(min=1), _unique_names),
            ),
            cv.Optional(CONF_BITS_PER_SAMPLE, default=PASSTHROUGH): cv.Any(
                cv.one_of(PASSTHROUGH, lower=True), cv.int_range(8, 32)
            ),
            cv.Optional(
                CONF_BUFFER_DURATION, default="100ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TASK_STACK_IN_PSRAM): psram.validate_task_stack_in_psram,
            cv.Optional(CONF_FILTERS, default=16): cv.int_range(min=2, max=1024),
            cv.Optional(CONF_TAPS, default=16): validate_taps,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    _set_stream_limits,
)


FINAL_VALIDATE_SCHEMA = _validate_audio_compatibility


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    if _is_output_list(config):
        groups = 0
        for output in config[CONF_OUTPUT_SPEAKER]:
            if _is_group(output):
                cg.add(
                    var.add_output_group(
                        output[CONF_SAMPLE_RATE],
                        output[CONF_BITS_PER_SAMPLE],
                        output[CONF_NUM_CHANNELS],
                    )
                )
                hub = await cg.get_variable(output[CONF_USB_AUDIO])
                cg.add(
                    hub.add_speaker_route(
                        cg.RawExpression(
                            "[](esphome::speaker::Speaker *sink, const char *name, uint32_t key, uint32_t rate, "
                            "uint8_t bits, uint8_t channels, bool added) { "
                            f"if (added) {{ {var}->add_group_output({groups}, sink, name, key, rate, bits, channels); }} "
                            f"else {{ {var}->remove_output(sink); }} }}"
                        )
                    )
                )
                groups += 1
                continue
            spk = await cg.get_variable(output[CONF_SPEAKER])
            cg.add(
                var.add_output(
                    spk,
                    output[CONF_NAME],
                    output[CONF_SAMPLE_RATE],
                    output[CONF_BITS_PER_SAMPLE],
                    output[CONF_NUM_CHANNELS],
                )
            )
        cg.add(var.set_preference_key(fnv1_hash(f"audio_router:{config[CONF_ID]}")))
    else:
        await cg.add_variable(var.set_output_speaker, config[CONF_OUTPUT_SPEAKER])

    cg.add(var.set_buffer_duration(config[CONF_BUFFER_DURATION]))

    if config.get(CONF_TASK_STACK_IN_PSRAM):
        cg.add(var.set_task_stack_in_psram(True))
        psram.request_external_task_stack()

    if config[CONF_BITS_PER_SAMPLE] == PASSTHROUGH:
        cg.add(var.set_passthrough_bits_per_sample(True))
    else:
        cg.add(var.set_target_bits_per_sample(config[CONF_BITS_PER_SAMPLE]))
    cg.add(var.set_target_sample_rate(config[CONF_SAMPLE_RATE]))

    cg.add(var.set_filters(config[CONF_FILTERS]))
    cg.add(var.set_taps(config[CONF_TAPS]))
