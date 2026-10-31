import esphome.codegen as cg
from esphome.components import audio_dac
import esphome.config_validation as cv
from esphome.const import CONF_ID

from .. import CONF_USB_AUDIO_ID, USBAudioClient, usb_audio_ns

USBAudioDac = usb_audio_ns.class_("USBAudioDac", audio_dac.AudioDac, cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(USBAudioDac),
        cv.GenerateID(CONF_USB_AUDIO_ID): cv.use_id(USBAudioClient),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_USB_AUDIO_ID])
