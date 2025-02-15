import esphome.codegen as cg
from esphome.components import output
import esphome.config_validation as cv
from esphome.const import CONF_ID

dynamic_lamp_ns = cg.esphome_ns.namespace('dynamic_lamp')
DynamicLamp = dynamic_lamp_ns.class_('DynamicLamp', cg.Component)

CONF_SAVE_MODE = 'save_mode'
CONF_AVAILABLE_OUTPUTS = 'available_outputs'
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(DynamicLamp),
    cv.Required(CONF_AVAILABLE_OUTPUTS): [cv.use_id(output.FloatOutput)],
    cv.Optional(CONF_SAVE_MODE, default=0): cv.int_range(0, 1),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for outputPointer in config.get(CONF_AVAILABLE_OUTPUTS, []):
        output_ = await cg.get_variable(outputPointer)
        cg.add(var.add_available_output(output_))
    cg.add(var.set_save_mode(config[CONF_SAVE_MODE]))
