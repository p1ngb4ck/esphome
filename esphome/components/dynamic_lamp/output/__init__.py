import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_OUTPUT_ID

from .. import CONF_DYNAMIC_LAMP_ID, DynamicLampComponent, dynamic_lamp_ns

DEPENDENCIES = ["dynamic_lamp"]

dynamic_monochromatic_ns = cg.esphome_ns.namespace("dynamic_monochromatic_light")
DynamicLamp = dynamic_monochromatic_ns.class_(
    "DynamicLamp", light.LightOutput, cg.Parented.template(DynamicLampComponent)
)

DynamicLampIdx = dynamic_lamp_ns.enum("DynamicLampIdx")
CHANNEL_OPTIONS = {
    "A": DynamicLampIdx.LAMP_1,
    "B": DynamicLampIdx.LAMP_2,
    "C": DynamicLampIdx.LAMP_3,
    "D": DynamicLampIdx.LAMP_4,
    "E": DynamicLampIdx.LAMP_5,
    "F": DynamicLampIdx.LAMP_6,
    "G": DynamicLampIdx.LAMP_7,
    "H": DynamicLampIdx.LAMP_8,
    "I": DynamicLampIdx.LAMP_9,
    "J": DynamicLampIdx.LAMP_10,
    "K": DynamicLampIdx.LAMP_11,
    "L": DynamicLampIdx.LAMP_12,
    "M": DynamicLampIdx.LAMP_13,
    "N": DynamicLampIdx.LAMP_14,
    "O": DynamicLampIdx.LAMP_15,
    "P": DynamicLampIdx.LAMP_16,
}

CONFIG_SCHEMA = light.BRIGHTNESS_ONLY_LIGHT_SCHEMA.extend(
    {
        cv.Required(CONF_OUTPUT_ID): cv.declare_id(DynamicLamp),
        cv.GenerateID(CONF_DYNAMIC_LAMP_ID): cv.use_id(DynamicLampComponent),
        cv.Required(CONF_CHANNEL): cv.enum(CHANNEL_OPTIONS, upper=True),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_DYNAMIC_LAMP_ID])
    var = cg.new_Pvariable(
        config[CONF_OUTPUT_ID],
        parent,
        config[CONF_CHANNEL],
    )
    await light.register_light(var, config)
    await cg.register_parented(var, config[CONF_DYNAMIC_LAMP_ID])
