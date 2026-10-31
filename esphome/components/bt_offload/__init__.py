import esphome.codegen as cg
from esphome.components.hid import HID_INPUT_SCHEMA, HIDInput, setup_hid_input
import esphome.config_validation as cv
from esphome.const import CONF_ID
import esphome.final_validate as fv

DEPENDENCIES = ["esp32", "esp32_hosted", "usb_bluetooth"]
AUTO_LOAD = ["hid"]

bt_offload_ns = cg.esphome_ns.namespace("bt_offload")
BTOffload = bt_offload_ns.class_("BTOffload", cg.Component, HIDInput)

CONFIG_SCHEMA = (
    cv.Schema({cv.GenerateID(): cv.declare_id(BTOffload)})
    .extend(HID_INPUT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config):
    dongles = fv.full_config.get().get("usb_bluetooth") or []
    if not any(d.get("c6_offload") for d in dongles):
        raise cv.Invalid("bt_offload needs a usb_bluetooth with c6_offload: true")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_hid_input(var, config)
    cg.add_define("USE_BT_OFFLOAD")
