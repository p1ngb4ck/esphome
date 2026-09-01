import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID as CONF_ID

from .. import CONF_A2DP_SOURCE_ID, A2DPSource, a2dp_source_ns

DEPENDENCIES = ["a2dp_source"]

A2DPDeviceSelect = a2dp_source_ns.class_(
    "A2DPDeviceSelect", select.Select, cg.Component
)

CONFIG_SCHEMA = (
    select.select_schema(A2DPDeviceSelect)
    .extend(
        {
            cv.GenerateID(CONF_A2DP_SOURCE_ID): cv.use_id(A2DPSource),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # The real options arrive at runtime, from whatever the pairing window finds.
    # A select has to be registered with something, so it starts with the
    # placeholder the entity also shows before the first scan.
    var = await select.new_select(config, options=["(no devices found)"])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_A2DP_SOURCE_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_device_select(var))
