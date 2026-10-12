"""Paired Bluetooth Classic and LE devices of bt_hid and ble_client_hid slots.

Kept across reboots in preferences, or in a file on a storage (file: a VFS path). The bond
keys themselves stay in Bluedroid's NVS store; forgetting a device also deletes its bond.
"""

import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32_ble_tracker
from esphome.components.globals import GlobalsComponent
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_DURATION,
    CONF_FILE,
    CONF_ID,
    CONF_RESTORE_VALUE,
    CONF_TRIGGER_ID,
    CONF_TYPE,
)
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]

_LOGGER = logging.getLogger(__name__)

CONF_BT_PAIRING_ID = "bt_pairing_id"
CONF_ADDRESS_GLOBAL = "address_global"
CONF_MAX_RESTORE_DATA_LENGTH = "max_restore_data_length"
CONF_MAX_DEVICES = "max_devices"
CONF_ON_STATUS = "on_status"

bt_pairing_ns = cg.esphome_ns.namespace("bt_pairing")
BTPairing = bt_pairing_ns.class_("BTPairing", cg.Component)
ForgetAllAction = bt_pairing_ns.class_(
    "ForgetAllAction", automation.Action, cg.Parented.template(BTPairing)
)
ForgetAction = bt_pairing_ns.class_(
    "ForgetAction", automation.Action, cg.Parented.template(BTPairing)
)
LogAction = bt_pairing_ns.class_(
    "LogAction", automation.Action, cg.Parented.template(BTPairing)
)
ScanAction = bt_pairing_ns.class_(
    "ScanAction", automation.Action, cg.Parented.template(BTPairing)
)
CancelScanAction = bt_pairing_ns.class_(
    "CancelScanAction", automation.Action, cg.Parented.template(BTPairing)
)
StatusTrigger = bt_pairing_ns.class_(
    "StatusTrigger", automation.Trigger.template(cg.std_string)
)


def _validate_file(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/") or value.count("/") < 2:
        raise cv.Invalid(
            "file is a path under a storage's mount_path, e.g. /sdcard/bt_pairing.bin"
        )
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BTPairing),
        cv.Optional(CONF_MAX_DEVICES, default=8): cv.int_range(min=1, max=32),
        # A file under a storage's mount_path; without it the database is kept in
        # preferences. Read once the storage is mounted, rewritten on every change.
        cv.Optional(CONF_FILE): _validate_file,
        # The pairing scan also lists advertising BLE HID devices.
        cv.OnlyWith(
            esp32_ble_tracker.CONF_ESP32_BLE_ID, "esp32_ble_tracker"
        ): cv.use_id(esp32_ble_tracker.ESP32BLETracker),
        # What the pairing scan and a pick do: message
        cv.Optional(CONF_ON_STATUS): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StatusTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_BT_PAIRING")
    cg.add(var.set_max_devices(config[CONF_MAX_DEVICES]))
    cg.add(var.set_preference_key(fnv1_hash(f"bt_pairing:{config[CONF_ID]}")))
    if CONF_FILE in config:
        cg.add_define("USE_BT_PAIRING_STORAGE")
        cg.add(var.set_file(config[CONF_FILE]))
    if esp32_ble_tracker.CONF_ESP32_BLE_ID in config:
        cg.add_define("USE_BT_PAIRING_LE")
        await esp32_ble_tracker.register_ble_device(var, config)
    for conf in config.get(CONF_ON_STATUS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "message")], conf)


def _final_validate(config):
    if CONF_FILE in config and "storage" not in fv.full_config.get():
        raise cv.Invalid(
            f"{CONF_FILE} needs a storage (sd_storage, usb_storage, ...) mounted there"
        )


FINAL_VALIDATE_SCHEMA = _final_validate


PAIRING_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(BTPairing)}
)


@automation.register_action(
    "bt_pairing.forget_all",
    ForgetAllAction,
    PAIRING_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "bt_pairing.log", LogAction, PAIRING_ACTION_SCHEMA, synchronous=True
)
async def pairing_simple_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "bt_pairing.cancel_scan", CancelScanAction, PAIRING_ACTION_SCHEMA, synchronous=True
)
async def cancel_scan_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "bt_pairing.scan",
    ScanAction,
    automation.maybe_simple_id(
        {
            cv.GenerateID(): cv.use_id(BTPairing),
            cv.Optional(CONF_DURATION, default="20s"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
    synchronous=True,
)
async def scan_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    duration = config[CONF_DURATION]
    if not cg.is_template(duration):
        duration = duration.total_milliseconds
    cg.add(var.set_duration(await cg.templatable(duration, args, cg.uint32)))
    return var


def _validate_address(value):
    if isinstance(value, cv.Lambda):
        return value
    return str(cv.mac_address(value))


@automation.register_action(
    "bt_pairing.forget",
    ForgetAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(BTPairing),
            cv.Required(CONF_ADDRESS): cv.templatable(_validate_address),
        }
    ),
    synchronous=True,
)
async def forget_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_address(await cg.templatable(config[CONF_ADDRESS], args, cg.std_string))
    )
    return var


# For bt_hid / ble_client_hid slots: a std::string global (restore_value: true) holding the
# paired device's address, "AA:BB:CC:DD:EE:FF" or "" -- upstream's globals and preferences,
# readable and settable from lambdas. Works alongside bt_pairing or without it.
ADDRESS_GLOBAL_SCHEMA = {
    cv.Optional(CONF_ADDRESS_GLOBAL): cv.use_id(GlobalsComponent),
}


def validate_address_global(config) -> None:
    """FINAL_VALIDATE part: the global is a std::string long enough for an address."""
    if CONF_ADDRESS_GLOBAL not in config:
        return
    gid = str(config[CONF_ADDRESS_GLOBAL])
    for glob in fv.full_config.get().get("globals") or []:
        if str(glob[CONF_ID]) != gid:
            continue
        if glob.get(CONF_TYPE) != "std::string":
            raise cv.Invalid(f"{CONF_ADDRESS_GLOBAL} '{gid}' must be type std::string")
        if not glob.get(CONF_RESTORE_VALUE):
            _LOGGER.warning(
                "%s '%s' has no restore_value: the paired device is lost on reboot",
                CONF_ADDRESS_GLOBAL,
                gid,
            )
        elif glob.get(CONF_MAX_RESTORE_DATA_LENGTH, 63) < 17:
            raise cv.Invalid(
                f"{CONF_ADDRESS_GLOBAL} '{gid}' needs max_restore_data_length >= 17"
            )
        return


async def setup_address_global(var, config) -> None:
    if CONF_ADDRESS_GLOBAL not in config:
        return
    glob = await cg.get_variable(config[CONF_ADDRESS_GLOBAL])
    cg.add(
        var.set_address_global(
            cg.RawExpression(f"[]() -> std::string {{ return {glob}->value(); }}"),
            cg.RawExpression(f"[](const std::string &v) {{ {glob}->value() = v; }}"),
        )
    )
