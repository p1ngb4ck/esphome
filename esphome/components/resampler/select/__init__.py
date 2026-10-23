import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME, CONF_OUTPUT_SPEAKER
from esphome.core import CORE

from .. import resampler_ns
from ..speaker import ResamplerSpeaker

DEPENDENCIES = ["speaker"]

CONF_RESAMPLER_ID = "resampler_id"

OutputSelect = resampler_ns.class_(
    "OutputSelect", select.Select, cg.Component, cg.Parented.template(ResamplerSpeaker)
)

CONFIG_SCHEMA = (
    select.select_schema(OutputSelect, icon="mdi:speaker-multiple")
    .extend(
        {
            cv.GenerateID(CONF_RESAMPLER_ID): cv.use_id(ResamplerSpeaker),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config):
    for conf in CORE.config.get("speaker") or []:
        if str(conf.get(CONF_ID)) == str(config[CONF_RESAMPLER_ID]):
            if not isinstance(conf.get(CONF_OUTPUT_SPEAKER), list):
                raise cv.Invalid(
                    "the resampler of an output select needs a list of outputs in output_speaker",
                    [CONF_RESAMPLER_ID],
                )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    resampler_id = config[CONF_RESAMPLER_ID]
    names = []
    for conf in CORE.config.get("speaker") or []:
        if str(conf.get(CONF_ID)) == str(resampler_id):
            names = [o[CONF_NAME] for o in conf[CONF_OUTPUT_SPEAKER]]
    var = await select.new_select(config, options=names)
    await cg.register_component(var, config)
    await cg.register_parented(var, resampler_id)
