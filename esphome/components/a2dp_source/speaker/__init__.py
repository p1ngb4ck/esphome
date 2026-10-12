import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE
import esphome.final_validate as fv

from .. import CONF_A2DP_SOURCE_ID, A2DPSource, a2dp_source_ns

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["a2dp_source"]
AUTO_LOAD = ["audio"]

A2DPSpeaker = a2dp_source_ns.class_(
    "A2DPSpeaker", speaker.Speaker, cg.Component, cg.Parented.template(A2DPSource)
)


def _set_stream_limits(config):
    # What A2DP's SBC encoder takes here; players resample to it.
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=2,
        max_channels=2,
        min_sample_rate=44100,
        max_sample_rate=44100,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(A2DPSpeaker),
            cv.GenerateID(CONF_A2DP_SOURCE_ID): cv.use_id(A2DPSource),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


def _final_validate(config):
    full = fv.full_config.get()
    parent = str(config[CONF_A2DP_SOURCE_ID])
    speakers = [
        c
        for c in full.get("speaker") or []
        if c.get("platform") == "a2dp_source"
        and str(c.get(CONF_A2DP_SOURCE_ID)) == parent
    ]
    if len(speakers) > 1:
        raise cv.Invalid("one a2dp_source speaker per a2dp_source")
    source = full.get("a2dp_source") or {}
    if str(source.get(CONF_ID)) == parent and CONF_MICROPHONE in source:
        raise cv.Invalid(
            "the a2dp_source this speaker plays into already takes its audio from a "
            "microphone; use one or the other"
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    await cg.register_parented(var, config[CONF_A2DP_SOURCE_ID])
