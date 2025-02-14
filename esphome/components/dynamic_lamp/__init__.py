import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

dynamic_lamp_ns = cg.esphome_ns.namespace('dynamic_lamp')
DynamicLamp = dynamic_lamp_ns.class_('DynamicLamp', cg.Component)

CONF_SAVE_MODE = 'save_mode'
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(DynamicLamp),
    cv.Optional(CONF_SAVE_MODE, default=0): cv.int_range(0, 1),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_save_mode(config[CONF_SAVE_MODE]))
