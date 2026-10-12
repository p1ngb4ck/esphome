"""Bluetooth Classic HID devices: keyboards, mice, game controllers and remotes.

Bluedroid's HID host on the BR/EDR controller, which is the USB dongle of usb_bluetooth
(bluedroid: true). Reports feed the shared hid input, as usb_hid and ble_client_hid do.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components.hid import HID_INPUT_SCHEMA, HIDInput, setup_hid_input
import esphome.config_validation as cv
from esphome.const import (
    CONF_DURATION,
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_ON_CONNECT,
    CONF_ON_DISCONNECT,
    CONF_PIN,
    CONF_TRIGGER_ID,
)
import esphome.final_validate as fv

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["hid"]
MULTI_CONF = True

CONF_ON_PASSKEY = "on_passkey"
CONF_AUTO_CONNECT = "auto_connect"

bt_hid_ns = cg.esphome_ns.namespace("bt_hid")
BTHIDDevice = bt_hid_ns.class_("BTHIDDevice", cg.Component, HIDInput)
PasskeyTrigger = bt_hid_ns.class_(
    "PasskeyTrigger", automation.Trigger.template(cg.uint32)
)
ConnectTrigger = bt_hid_ns.class_("ConnectTrigger", automation.Trigger.template())
DisconnectTrigger = bt_hid_ns.class_("DisconnectTrigger", automation.Trigger.template())
ConnectAction = bt_hid_ns.class_("ConnectAction", automation.Action)
DisconnectAction = bt_hid_ns.class_("DisconnectAction", automation.Action)
UnpairAction = bt_hid_ns.class_("UnpairAction", automation.Action)
ScanAction = bt_hid_ns.class_("ScanAction", automation.Action)


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
        # Off: connected only by bt_hid.connect; the device connecting on its own is refused.
        cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
        cv.Optional(CONF_ON_CONNECT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ConnectTrigger)}
        ),
        cv.Optional(CONF_ON_DISCONNECT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DisconnectTrigger)}
        ),
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
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
    for conf in config.get(CONF_ON_PASSKEY, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.uint32, "passkey")], conf)
    for key in (CONF_ON_CONNECT, CONF_ON_DISCONNECT):
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)

    add_idf_sdkconfig_option("CONFIG_BT_HID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_HID_HOST_ENABLED", True)


DEVICE_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.Required(CONF_ID): cv.use_id(BTHIDDevice)}
)

# Connect, and keep the device connected (it may also connect on its own again).
automation.register_simple_action(
    "bt_hid.connect", ConnectAction, DEVICE_ACTION_SCHEMA, synchronous=True
)
# Disconnect, and keep it disconnected until bt_hid.connect.
automation.register_simple_action(
    "bt_hid.disconnect", DisconnectAction, DEVICE_ACTION_SCHEMA, synchronous=True
)
# Delete the pairing; the device has to be put in pairing mode to connect again.
automation.register_simple_action(
    "bt_hid.unpair", UnpairAction, DEVICE_ACTION_SCHEMA, synchronous=True
)


def _validate_scan_duration(value):
    value = cv.positive_time_period_milliseconds(value)
    if not 1280 <= value.total_milliseconds <= 61440:
        raise cv.Invalid("a scan lasts 1.28 s to 61.44 s")
    return value


# Looks for discoverable Bluetooth Classic HID devices and logs their addresses, for every
# bt_hid device at once.
@automation.register_action(
    "bt_hid.scan",
    ScanAction,
    cv.Schema(
        {
            cv.Optional(CONF_DURATION, default="10s"): cv.templatable(
                _validate_scan_duration
            ),
        }
    ),
    synchronous=True,
)
async def scan_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    duration = config[CONF_DURATION]
    if not cg.is_template(duration):
        duration = duration.total_milliseconds
    cg.add(var.set_duration(await cg.templatable(duration, args, cg.uint32)))
    return var
