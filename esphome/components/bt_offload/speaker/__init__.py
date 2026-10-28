import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import CONF_BITS_PER_SAMPLE, CONF_ID, CONF_NUM_CHANNELS, CONF_SAMPLE_RATE
import esphome.final_validate as fv

from .. import BTOffload, bt_offload_ns

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["bt_offload"]
AUTO_LOAD = ["audio", "ring_buffer"]

CONF_BT_OFFLOAD_ID = "bt_offload_id"

BTOffloadSpeaker = bt_offload_ns.class_(
    "BTOffloadSpeaker", speaker.Speaker, cg.Component, cg.Parented.template(BTOffload)
)

_FORMAT = {CONF_BITS_PER_SAMPLE: 16, CONF_NUM_CHANNELS: 2, CONF_SAMPLE_RATE: 44100}


def _set_stream_limits(config):
    for key, value in _FORMAT.items():
        if config.setdefault(key, value) != value:
            raise cv.Invalid(f"{key} is {value} for a bt_offload speaker")
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
            cv.GenerateID(): cv.declare_id(BTOffloadSpeaker),
            cv.GenerateID(CONF_BT_OFFLOAD_ID): cv.use_id(BTOffload),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


def _final_validate(config):
    parent = str(config[CONF_BT_OFFLOAD_ID])
    speakers = [
        c
        for c in fv.full_config.get().get("speaker") or []
        if c.get("platform") == "bt_offload" and str(c.get(CONF_BT_OFFLOAD_ID)) == parent
    ]
    if len(speakers) > 1:
        raise cv.Invalid("one bt_offload speaker per bt_offload")


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    await cg.register_parented(var, config[CONF_BT_OFFLOAD_ID])
    cg.add_define("USE_BT_OFFLOAD_SPEAKER")
