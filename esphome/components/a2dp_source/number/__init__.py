import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG, UNIT_MILLISECOND

from .. import CONF_A2DP_SOURCE_ID, A2DPSource, a2dp_source_ns

DEPENDENCIES = ["a2dp_source"]

A2DPDelayNumber = a2dp_source_ns.class_(
    "A2DPDelayNumber", number.Number, cg.Parented.template(A2DPSource)
)

# The connected device's output delay, kept per device (A2DPSource::MAX_OUTPUT_DELAY_MS).
MAX_DELAY_MS = 3000

CONFIG_SCHEMA = number.number_schema(
    A2DPDelayNumber,
    unit_of_measurement=UNIT_MILLISECOND,
    entity_category=ENTITY_CATEGORY_CONFIG,
    icon="mdi:timer-sand",
).extend({cv.GenerateID(CONF_A2DP_SOURCE_ID): cv.use_id(A2DPSource)})


async def to_code(config):
    var = await number.new_number(config, min_value=0, max_value=MAX_DELAY_MS, step=10)
    parent = await cg.get_variable(config[CONF_A2DP_SOURCE_ID])
    await cg.register_parented(var, parent)
    cg.add(parent.set_delay_number(var))
    cg.add_define("USE_A2DP_SOURCE_NUMBER")
