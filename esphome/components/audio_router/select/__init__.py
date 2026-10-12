import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME
from esphome.core import CORE

from .. import CONF_AUDIO_ROUTER_ID, audio_router_ns
from ..speaker import CONF_OUTPUTS, AudioRouter

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["speaker"]

AudioOutputSelect = audio_router_ns.class_(
    "AudioOutputSelect", select.Select, cg.Component, cg.Parented.template(AudioRouter)
)

CONFIG_SCHEMA = (
    select.select_schema(AudioOutputSelect, icon="mdi:speaker-multiple")
    .extend(
        {
            cv.GenerateID(CONF_AUDIO_ROUTER_ID): cv.use_id(AudioRouter),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    router_id = config[CONF_AUDIO_ROUTER_ID]
    names = []
    for conf in CORE.config.get("speaker") or []:
        if str(conf.get(CONF_ID)) == str(router_id):
            names = [o[CONF_NAME] for o in conf[CONF_OUTPUTS]]
    var = await select.new_select(config, options=names)
    await cg.register_component(var, config)
    await cg.register_parented(var, router_id)
