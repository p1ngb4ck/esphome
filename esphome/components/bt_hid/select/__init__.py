import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from .. import BTHIDDevice, bt_hid_ns

DEPENDENCIES = ["bt_hid"]

CONF_BT_HID_ID = "bt_hid_id"

BTHIDDeviceSelect = bt_hid_ns.class_("BTHIDDeviceSelect", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(BTHIDDeviceSelect, icon="mdi:bluetooth-connect")
    .extend({cv.GenerateID(CONF_BT_HID_ID): cv.use_id(BTHIDDevice)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # The options are the devices a pairing scan finds, at runtime.
    var = await select.new_select(config, options=["(no devices found)"])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_BT_HID_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_device_select(var))
    cg.add_define("USE_BT_HID_SELECT")
