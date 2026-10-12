"""Bluetooth Classic HID devices: keyboards, mice, game controllers and remotes.

Bluedroid's HID host on the BR/EDR controller, which is the USB dongle of usb_bluetooth
(bluedroid: true). Reports feed the shared hid input, as usb_hid and ble_client_hid do.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components.hid import HID_INPUT_SCHEMA, HIDInput, setup_hid_input
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MAC_ADDRESS, CONF_PIN, CONF_TRIGGER_ID
import esphome.final_validate as fv

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["hid"]
MULTI_CONF = True

CONF_ON_PASSKEY = "on_passkey"

bt_hid_ns = cg.esphome_ns.namespace("bt_hid")
BTHIDDevice = bt_hid_ns.class_("BTHIDDevice", cg.Component, HIDInput)
PasskeyTrigger = bt_hid_ns.class_(
    "PasskeyTrigger", automation.Trigger.template(cg.uint32)
)


def _validate_pin(value):
    value = cv.string_strict(value)
    if not 1 <= len(value) <= 16 or not value.isdigit():
        raise cv.Invalid("a PIN is 1 to 16 digits")
    return value


# Keys, text, pointer and controls: see hid.HID_INPUT_SCHEMA. Keys as binary sensors or
# events and typed text are the hid platforms, given this device's id as hid_id.
CONFIG_SCHEMA = cv.COMPONENT_SCHEMA.extend(HID_INPUT_SCHEMA).extend(
    {
        cv.GenerateID(): cv.declare_id(BTHIDDevice),
        cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
        # Legacy pairing (older keyboards): this PIN is typed on the keyboard, then Enter.
        cv.Optional(CONF_PIN, default="0000"): _validate_pin,
        # Simple pairing with a keyboard: the passkey to type on it, then Enter.
        cv.Optional(CONF_ON_PASSKEY): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PasskeyTrigger)}
        ),
    }
)


def _final_validate(config):
    full = fv.full_config.get()
    if not any(c.get("bluedroid") for c in full.get("usb_bluetooth") or []):
        raise cv.Invalid(
            "bt_hid needs a Bluetooth Classic controller: a usb_bluetooth dongle with "
            "bluedroid: true"
        )
    if "a2dp_source" in full:
        raise cv.Invalid(
            "bt_hid and a2dp_source both register Bluedroid's Classic GAP callback"
        )
    macs = [str(c[CONF_MAC_ADDRESS]) for c in full.get("bt_hid") or []]
    if len(macs) != len(set(macs)):
        raise cv.Invalid("two bt_hid entries have the same mac_address")


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_hid_input(var, config)
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))
    cg.add(var.set_pin(config[CONF_PIN]))
    for conf in config.get(CONF_ON_PASSKEY, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.uint32, "passkey")], conf)

    add_idf_sdkconfig_option("CONFIG_BT_HID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_HID_HOST_ENABLED", True)
