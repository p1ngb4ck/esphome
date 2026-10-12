"""BLE devices that only advertise (buttons, remotes), heard without a connection.

Each entry is a slot: bt_pairing.scan lists a device whose advertised data changes while the
scan runs (press a button on it) as [BLE broadcast]; picking it fills a free slot, kept in
bt_pairing. on_manufacturer_data / on_service_data fire whenever its advertisement changes.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32_ble_tracker
from esphome.components.bt_pairing import (
    ADDRESS_GLOBAL_SCHEMA,
    CONF_BT_PAIRING_ID,
    BTPairing,
    setup_address_global,
    validate_address_global,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32", "bt_pairing", "esp32_ble_tracker"]
MULTI_CONF = True

CONF_ON_MANUFACTURER_DATA = "on_manufacturer_data"
CONF_ON_SERVICE_DATA = "on_service_data"

ble_broadcast_ns = cg.esphome_ns.namespace("ble_broadcast")
BLEBroadcast = ble_broadcast_ns.class_(
    "BLEBroadcast", cg.Component, esp32_ble_tracker.ESPBTDeviceListener
)
ManufacturerDataTrigger = ble_broadcast_ns.class_(
    "ManufacturerDataTrigger",
    automation.Trigger.template(cg.std_vector.template(cg.uint8), cg.uint16),
)
ServiceDataTrigger = ble_broadcast_ns.class_(
    "ServiceDataTrigger",
    automation.Trigger.template(cg.std_vector.template(cg.uint8), cg.uint16),
)
UnpairAction = ble_broadcast_ns.class_(
    "UnpairAction", automation.Action, cg.Parented.template(BLEBroadcast)
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BLEBroadcast),
            cv.GenerateID(CONF_BT_PAIRING_ID): cv.use_id(BTPairing),
            **ADDRESS_GLOBAL_SCHEMA,
            # x: the data after the company id, id: the company id
            cv.Optional(CONF_ON_MANUFACTURER_DATA): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ManufacturerDataTrigger)}
            ),
            # x: the service data, uuid: its 16-bit UUID (0 for a longer one)
            cv.Optional(CONF_ON_SERVICE_DATA): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ServiceDataTrigger)}
            ),
        }
    )
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config):
    validate_address_global(config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)
    cg.add(var.set_pairing(await cg.get_variable(config[CONF_BT_PAIRING_ID])))
    cg.add(var.set_slot(fnv1_hash(f"ble_broadcast:{config[CONF_ID]}")))
    await setup_address_global(var, config)
    data = cg.std_vector.template(cg.uint8)
    for conf in config.get(CONF_ON_MANUFACTURER_DATA, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(data, "x"), (cg.uint16, "id")], conf
        )
    for conf in config.get(CONF_ON_SERVICE_DATA, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(data, "x"), (cg.uint16, "uuid")], conf
        )


@automation.register_action(
    "ble_broadcast.unpair",
    UnpairAction,
    automation.maybe_simple_id({cv.GenerateID(): cv.use_id(BLEBroadcast)}),
    synchronous=True,
)
async def unpair_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
