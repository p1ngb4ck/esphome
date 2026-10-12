import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv

from .. import SimpleVideoPlayer, simple_video_player_ns

DEPENDENCIES = ["simple_video_player"]

CONF_SIMPLE_VIDEO_PLAYER_ID = "simple_video_player_id"

VolumeNumber = simple_video_player_ns.class_("VolumeNumber", number.Number, cg.Component)

CONFIG_SCHEMA = (
    number.number_schema(VolumeNumber)
    .extend({cv.GenerateID(CONF_SIMPLE_VIDEO_PLAYER_ID): cv.use_id(SimpleVideoPlayer)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await number.new_number(config, min_value=0, max_value=100, step=1)
    await cg.register_component(var, config)
    cg.add(var.set_parent(await cg.get_variable(config[CONF_SIMPLE_VIDEO_PLAYER_ID])))
