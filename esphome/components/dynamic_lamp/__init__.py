import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

dynamic_lamp_ns = cg.esphome_ns.namespace('dynamic_lamp')
DynamicLamp = dynamic_lamp_ns.class_('DynamicLamp', cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(DynamicLamp)
}).extend(cv.COMPONENT_SCHEMA)

def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)
