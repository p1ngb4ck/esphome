import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_ID,
    CONF_NAME,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)
from esphome.helpers import fnv1_hash

from .. import audio_router_ns

CODEOWNERS = ["@p1ngb4ck"]
AUTO_LOAD = ["audio"]

CONF_OUTPUTS = "outputs"

AudioRouter = audio_router_ns.class_("AudioRouter", cg.Component, speaker.Speaker)

_DEFAULT_FORMAT = {
    CONF_BITS_PER_SAMPLE: 16,
    CONF_NUM_CHANNELS: 2,
    CONF_SAMPLE_RATE: 48000,
}


def _set_format(config):
    # The format players write in, and every output has to take: put a resampler speaker in
    # front of an output that needs another one.
    for key, value in _DEFAULT_FORMAT.items():
        config.setdefault(key, value)
    audio.set_stream_limits(
        min_bits_per_sample=config[CONF_BITS_PER_SAMPLE],
        max_bits_per_sample=config[CONF_BITS_PER_SAMPLE],
        min_channels=config[CONF_NUM_CHANNELS],
        max_channels=config[CONF_NUM_CHANNELS],
        min_sample_rate=config[CONF_SAMPLE_RATE],
        max_sample_rate=config[CONF_SAMPLE_RATE],
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
    }
)

CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(AudioRouter),
            cv.Required(CONF_OUTPUTS): cv.All(
                cv.ensure_list(OUTPUT_SCHEMA), cv.Length(min=1), _unique_names
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _set_format,
)


def _final_validate(config):
    for output in config[CONF_OUTPUTS]:
        audio.final_validate_audio_schema(
            f"audio_router output '{output[CONF_NAME]}'",
            audio_device=CONF_SPEAKER,
            bits_per_sample=config[CONF_BITS_PER_SAMPLE],
            channels=config[CONF_NUM_CHANNELS],
            sample_rate=config[CONF_SAMPLE_RATE],
            audio_device_issue=True,
        )(output)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    cg.add(var.set_preference_key(fnv1_hash(f"audio_router:{config[CONF_ID]}")))
    for output in config[CONF_OUTPUTS]:
        spk = await cg.get_variable(output[CONF_SPEAKER])
        cg.add(var.add_output(spk, output[CONF_NAME]))
