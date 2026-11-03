import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from .. import CONF_BT_HOST_ID, BTHost, bt_host_ns

DEPENDENCIES = ["bt_host"]

BTHostSelect = bt_host_ns.class_("BTHostSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(BTHostSelect, icon="mdi:bluetooth-connect")
    .extend({cv.GenerateID(CONF_BT_HOST_ID): cv.use_id(BTHost)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # The options are the devices bt_host.scan finds, at runtime.
    var = await select.new_select(config, options=["(no devices found)"])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_BT_HOST_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_device_select(var))
    cg.add_define("USE_BT_PAIRING_SELECT")
