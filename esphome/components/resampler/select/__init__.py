import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_NAME, CONF_OUTPUT_SPEAKER
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash

from .. import resampler_ns
from ..microphone import ResamplerMicrophone
from ..speaker import ResamplerSpeaker

DEPENDENCIES = ["speaker"]

CONF_RESAMPLER_ID = "resampler_id"
CONF_MICROPHONE_ID = "microphone_id"

OutputSelect = resampler_ns.class_(
    "OutputSelect", select.Select, cg.Component, cg.Parented.template(ResamplerSpeaker)
)
SourceSelect = resampler_ns.class_(
    "SourceSelect", select.Select, cg.Component, cg.Parented.template(ResamplerMicrophone)
)

CONFIG_SCHEMA = cv.Any(
    select.select_schema(OutputSelect, icon="mdi:speaker-multiple")
    .extend(
        {
            cv.Required(CONF_RESAMPLER_ID): cv.use_id(ResamplerSpeaker),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    select.select_schema(SourceSelect, icon="mdi:microphone-settings")
    .extend(
        {
            cv.Required(CONF_MICROPHONE_ID): cv.use_id(ResamplerMicrophone),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
)


def _final_validate(config):
    if CONF_RESAMPLER_ID not in config:
        return config
    for conf in fv.full_config.get().get("speaker") or []:
        if str(conf.get(CONF_ID)) == str(config[CONF_RESAMPLER_ID]):
            if not isinstance(conf.get(CONF_OUTPUT_SPEAKER), list):
                raise cv.Invalid(
                    "the resampler of an output select needs a list of outputs in output_speaker",
                    [CONF_RESAMPLER_ID],
                )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    if CONF_MICROPHONE_ID in config:
        mic_id = config[CONF_MICROPHONE_ID]
        names = []
        for conf in CORE.config.get(CONF_MICROPHONE) or []:
            if str(conf.get(CONF_ID)) == str(mic_id):
                names = [
                    source.get(CONF_NAME, str(source[CONF_MICROPHONE]))
                    for source in conf[CONF_MICROPHONE]
                    if CONF_MICROPHONE in source
                ]
        cg.add_define("USE_RESAMPLER_SOURCE_SELECT")
        var = await select.new_select(config, options=names)
        await cg.register_component(var, config)
        await cg.register_parented(var, mic_id)
        parent = await cg.get_variable(mic_id)
        cg.add(parent.set_preference_key(fnv1_hash(f"resampler_microphone:{mic_id}")))
        return
    resampler_id = config[CONF_RESAMPLER_ID]
    names = []
    for conf in CORE.config.get("speaker") or []:
        if str(conf.get(CONF_ID)) == str(resampler_id):
            names = [o[CONF_NAME] for o in conf[CONF_OUTPUT_SPEAKER] if CONF_NAME in o]
    var = await select.new_select(config, options=names)
    await cg.register_component(var, config)
    await cg.register_parented(var, resampler_id)
