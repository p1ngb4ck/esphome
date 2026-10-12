import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from .. import SimpleVideoPlayer, simple_video_player_ns

DEPENDENCIES = ["simple_video_player"]

CONF_SIMPLE_VIDEO_PLAYER_ID = "simple_video_player_id"

StatusTextSensor = simple_video_player_ns.class_("StatusTextSensor", text_sensor.TextSensor, cg.Component)

# What svp_relay reads to find the panel and follow its sound: stream port, audio state, volume, mute.
CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(
        StatusTextSensor, entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:television-play"
    )
    .extend({cv.GenerateID(CONF_SIMPLE_VIDEO_PLAYER_ID): cv.use_id(SimpleVideoPlayer)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    cg.add(var.set_parent(await cg.get_variable(config[CONF_SIMPLE_VIDEO_PLAYER_ID])))
