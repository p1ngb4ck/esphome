import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from .. import BLEClientHID, ble_client_hid_ns

DEPENDENCIES = ["ble_client_hid"]

CONF_BLE_CLIENT_HID_ID = "ble_client_hid_id"

BLEHIDDeviceSelect = ble_client_hid_ns.class_(
    "BLEHIDDeviceSelect", select.Select, cg.Component
)

CONFIG_SCHEMA = (
    select.select_schema(BLEHIDDeviceSelect, icon="mdi:bluetooth-connect")
    .extend({cv.GenerateID(CONF_BLE_CLIENT_HID_ID): cv.use_id(BLEClientHID)})
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # The options are the HID devices a pairing window sees, at runtime.
    var = await select.new_select(config, options=["(no devices found)"])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.set_device_select(var))
    cg.add_define("USE_BLE_CLIENT_HID_SELECT")
