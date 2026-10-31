import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from .. import CONF_BT_PAIRING_ID, BTPairing, bt_pairing_ns

DEPENDENCIES = ["bt_pairing"]

BTPairingSelect = bt_pairing_ns.class_("BTPairingSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(BTPairingSelect, icon="mdi:bluetooth-connect")
    .extend({cv.GenerateID(CONF_BT_PAIRING_ID): cv.use_id(BTPairing)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # The options are the devices bt_pairing.scan finds, at runtime.
    var = await select.new_select(config, options=["(no devices found)"])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_BT_PAIRING_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_device_select(var))
    cg.add_define("USE_BT_PAIRING_SELECT")
