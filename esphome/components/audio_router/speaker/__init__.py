import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_CHANNEL,
    CONF_ID,
    CONF_NAME,
    CONF_NUM_CHANNELS,
    CONF_OUTPUT_SPEAKER,
    CONF_PLATFORM,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash

from .. import audio_router_ns

CODEOWNERS = ["@p1ngb4ck"]
AUTO_LOAD = ["audio"]

CONF_OUTPUTS = "outputs"
CONF_SOURCE_SPEAKERS = "source_speakers"

AudioRouter = audio_router_ns.class_("AudioRouter", cg.Component, speaker.Speaker)

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
    """What a speaker's own config fixes of its format: its keys, else equal min/max limits."""
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
    """The format a speaker plays natively: its own config, then down its output_speaker chain
    (a mixer's source speaker through its mixer). Keys missing at the end stay missing."""
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


def resolve_rate_range(fconf, speaker_id, native: int) -> tuple[int, int]:
    """The sample rates a speaker takes as they are: the limits of the first speaker down its
    output_speaker chain that sets them. A resampler plays its own rate, whatever comes in."""
    limit_low, limit_high = _LIMIT_KEYS[CONF_SAMPLE_RATE]
    seen = set()
    while speaker_id is not None and str(speaker_id) not in seen:
        seen.add(str(speaker_id))
        path = fconf.get_path_for_id(speaker_id)[:-1]
        confs = [fconf.get_config_for_path(path)]
        if len(path) >= 2 and path[-2] == CONF_SOURCE_SPEAKERS:
            confs.append(fconf.get_config_for_path(path[:-2]))
        speaker_id = None
        for conf in confs:
            if conf.get(CONF_PLATFORM) == "resampler":
                return native, native
            low = _number(conf.get(limit_low))
            high = _number(conf.get(limit_high))
            if low is not None and high is not None:
                return min(low, native), max(high, native)
            if speaker_id is None:
                speaker_id = conf.get(CONF_OUTPUT_SPEAKER)
    return native, native


def output_formats(fconf, router_conf) -> list[dict]:
    """Each output's native format: what is set on the output, else what its speaker plays; with
    the range of sample rates it takes (min_sample_rate, max_sample_rate)."""
    formats = []
    for output in router_conf[CONF_OUTPUTS]:
        known = {key: output[key] for key in FORMAT_KEYS if key in output}
        fmt = resolve_speaker_format(fconf, output[CONF_SPEAKER], known)
        if missing := [key for key in FORMAT_KEYS if key not in fmt]:
            raise cv.Invalid(
                f"audio_router output '{output[CONF_NAME]}': its speaker does not fix "
                f"{', '.join(missing)} -- set {', '.join(missing)} on the output"
            )
        fmt[audio.CONF_MIN_SAMPLE_RATE], fmt[audio.CONF_MAX_SAMPLE_RATE] = (
            resolve_rate_range(fconf, output[CONF_SPEAKER], fmt[CONF_SAMPLE_RATE])
        )
        formats.append(fmt)
    return formats


def _set_limits(config):
    # Channels and sample width are converted to the selected output's; the sample rate is passed on.
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=32,
        min_channels=1,
        max_channels=8,
    )(config)
    return config


def _unique_names(outputs):
    names = [o[CONF_NAME] for o in outputs]
    if len(names) != len(set(names)):
        raise cv.Invalid("output names must be unique")
    return outputs


OUTPUT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=32)),
        # The output's native format; taken from its speaker when left out.
        cv.Optional(CONF_SAMPLE_RATE): cv.int_range(min=8000, max=192000),
        cv.Optional(CONF_BITS_PER_SAMPLE): cv.one_of(16, 24, 32, int=True),
        cv.Optional(CONF_NUM_CHANNELS): cv.int_range(min=1, max=8),
    }
)

CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AudioRouter),
            # The format a speaker in front of the router (a mixer, a resampler) takes from
            # here when it leaves its own unset. The router itself takes any format.
            cv.Optional(CONF_SAMPLE_RATE, default=48000): cv.int_range(
                min=8000, max=192000
            ),
            cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(
                16, 24, 32, int=True
            ),
            cv.Optional(CONF_NUM_CHANNELS, default=2): cv.int_range(min=1, max=8),
            cv.Required(CONF_OUTPUTS): cv.All(
                cv.ensure_list(OUTPUT_SCHEMA), cv.Length(min=1), _unique_names
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _set_limits,
)


def _final_validate(config):
    fconf = fv.full_config.get()
    formats = output_formats(fconf, config)
    path = fconf.get_path_for_id(config[CONF_ID])[:-1]
    this_conf = fconf.get_config_for_path(path)
    for output, fmt in zip(this_conf[CONF_OUTPUTS], formats, strict=True):
        for key in FORMAT_KEYS:
            if fmt[key] not in (16, 24, 32) and key == CONF_BITS_PER_SAMPLE:
                raise cv.Invalid(
                    f"audio_router output '{output[CONF_NAME]}': {fmt[key]} bits per sample "
                    "is not supported (16, 24 or 32)"
                )
            output[key] = fmt[key]
        output[audio.CONF_MIN_SAMPLE_RATE] = fmt[audio.CONF_MIN_SAMPLE_RATE]
        output[audio.CONF_MAX_SAMPLE_RATE] = fmt[audio.CONF_MAX_SAMPLE_RATE]


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    cg.add(var.set_preference_key(fnv1_hash(f"audio_router:{config[CONF_ID]}")))
    for output in config[CONF_OUTPUTS]:
        spk = await cg.get_variable(output[CONF_SPEAKER])
        cg.add(
            var.add_output(
                spk,
                output[CONF_NAME],
                output[CONF_BITS_PER_SAMPLE],
                output[CONF_NUM_CHANNELS],
                output[CONF_SAMPLE_RATE],
                output[audio.CONF_MIN_SAMPLE_RATE],
                output[audio.CONF_MAX_SAMPLE_RATE],
            )
        )
