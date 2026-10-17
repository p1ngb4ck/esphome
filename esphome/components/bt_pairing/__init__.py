"""Bluetooth devices: one scan for every kind, and the devices paired from it.

max_hid_devices / max_broadcast_devices create the bt_hid, ble_client_hid and ble_broadcast
slots internally; a2dp_source joins on its own. Paired devices are kept across reboots in
preferences, or in a file on a storage (file: a VFS path); the bond keys stay in Bluedroid's
NVS store, and forgetting a device also deletes its bond.
"""

import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, esp32_ble, esp32_ble_tracker, hid
from esphome.components.storage import request_storage_worker
from esphome.components.globals import GlobalsComponent
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_DURATION,
    CONF_FILE,
    CONF_ID,
    CONF_KEY,
    CONF_MAX_CONNECTIONS,
    CONF_RESTORE_VALUE,
    CONF_TRIGGER_ID,
    CONF_TYPE,
)
from esphome.core import CORE
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
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_MAX_HID_DEVICES = "max_hid_devices"
CONF_MAX_BROADCAST_DEVICES = "max_broadcast_devices"
CONF_SLOTS = "slots"

bt_pairing_ns = cg.esphome_ns.namespace("bt_pairing")
# A HIDInput too (with max_hid_devices): hid_id for LVGL and the hid platforms.
BTPairing = bt_pairing_ns.class_("BTPairing", cg.Component, hid.HIDInput)
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
ConnectionTrigger = bt_pairing_ns.class_(
    "ConnectionTrigger", automation.Trigger.template(cg.std_string)
)
DeviceKeyTrigger = bt_pairing_ns.class_(
    "DeviceKeyTrigger",
    automation.Trigger.template(cg.std_string, cg.std_string, cg.bool_),
)
DeviceTextTrigger = bt_pairing_ns.class_(
    "DeviceTextTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)
DevicePointerTrigger = bt_pairing_ns.class_(
    "DevicePointerTrigger",
    automation.Trigger.template(
        cg.std_string, cg.int_, cg.int_, cg.int_, cg.int_, cg.int_
    ),
)
DeviceControlTrigger = bt_pairing_ns.class_(
    "DeviceControlTrigger",
    automation.Trigger.template(cg.std_string, cg.std_string, cg.int_),
)
PairAction = bt_pairing_ns.class_(
    "PairAction", automation.Action, cg.Parented.template(BTPairing)
)
ConnectAction = bt_pairing_ns.class_(
    "ConnectAction", automation.Action, cg.Parented.template(BTPairing)
)
DisconnectAction = bt_pairing_ns.class_(
    "DisconnectAction", automation.Action, cg.Parented.template(BTPairing)
)
UnpairAction = bt_pairing_ns.class_(
    "UnpairAction", automation.Action, cg.Parented.template(BTPairing)
)


def AUTO_LOAD(config=None) -> list[str]:
    """The components behind the internal slots."""
    slots = (config or {}).get(CONF_SLOTS) or {}
    loads = []
    if (config or {}).get(CONF_MAX_HID_DEVICES):
        loads.append("hid")
    if slots.get("bt_hid"):
        loads += ["bt_hid", "bt_classic"]
    if slots.get("ble_client"):
        loads += ["ble_client", "esp32_ble_client", "ble_client_hid", "sha256"]
    if slots.get("ble_broadcast"):
        loads.append("ble_broadcast")
    return loads


def _validate_file(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/") or value.count("/") < 2:
        raise cv.Invalid(
            "file is a path under a storage's mount_path, e.g. /sdcard/bt_pairing.bin"
        )
    return value


def _automation(trigger):
    return automation.validate_automation(
        {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(trigger)}
    )


def _ble_connection_budget() -> int:
    raw = (CORE.raw_config or {}).get("esp32_ble") or {}
    try:
        return int(raw.get(CONF_MAX_CONNECTIONS, esp32_ble.DEFAULT_MAX_CONNECTIONS))
    except (AttributeError, TypeError, ValueError):
        return esp32_ble.DEFAULT_MAX_CONNECTIONS


def _generate_slots(config):
    """The slots, as the configs of their own components, so IDs and BLE connection slots are
    accounted for like any other. Classic HID needs usb_bluetooth; BLE needs
    esp32_ble_tracker, and gets at most esp32_ble max_connections HID slots."""
    from esphome.components import ble_broadcast, ble_client, ble_client_hid, bt_hid

    hid_count = config[CONF_MAX_HID_DEVICES]
    slots = {}
    if hid_count and "usb_bluetooth" in CORE.loaded_integrations:
        slots["bt_hid"] = [
            bt_hid.CONFIG_SCHEMA({CONF_ID: f"bt_pairing_bt_hid_{i}"})
            for i in range(1, hid_count + 1)
        ]
    if "esp32_ble_tracker" in CORE.loaded_integrations:
        clients, nodes = [], []
        for i in range(1, min(hid_count, _ble_connection_budget()) + 1):
            client_id = f"bt_pairing_ble_client_{i}"
            clients.append(ble_client.CONFIG_SCHEMA({CONF_ID: client_id}))
            nodes.append(
                ble_client_hid.CONFIG_SCHEMA(
                    {
                        CONF_ID: f"bt_pairing_ble_hid_{i}",
                        ble_client.CONF_BLE_CLIENT_ID: client_id,
                    }
                )
            )
        if clients:
            slots["ble_client"] = clients
            slots["ble_client_hid"] = nodes
        if config[CONF_MAX_BROADCAST_DEVICES]:
            slots["ble_broadcast"] = [
                ble_broadcast.CONFIG_SCHEMA({CONF_ID: f"bt_pairing_ble_broadcast_{i}"})
                for i in range(1, config[CONF_MAX_BROADCAST_DEVICES] + 1)
            ]
    config[CONF_SLOTS] = slots
    return config


_BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BTPairing),
        cv.Optional(CONF_MAX_DEVICES, default=8): cv.int_range(min=1, max=32),
        # HID devices (keyboards, mice, remotes, game controllers) paired at most, Bluetooth
        # Classic and LE together. 0: no internal HID slots.
        cv.Optional(CONF_MAX_HID_DEVICES, default=0): cv.int_range(min=0, max=8),
        # BLE devices that only advertise (buttons), heard without a connection.
        cv.Optional(CONF_MAX_BROADCAST_DEVICES, default=0): cv.int_range(min=0, max=8),
        # Typed text of every HID device.
        cv.Optional(hid.CONF_LAYOUT, default="us"): hid.LAYOUT_SCHEMA,
        cv.Optional(hid.CONF_NUM_LOCK, default=True): cv.boolean,
        # device, key, pressed
        cv.Optional(hid.CONF_ON_KEY): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DeviceKeyTrigger),
                cv.Optional(CONF_KEY): hid.validate_key,
            }
        ),
        # device, text
        cv.Optional(hid.CONF_ON_TEXT): _automation(DeviceTextTrigger),
        # device, dx, dy, wheel, pan, buttons
        cv.Optional(hid.CONF_ON_POINTER): _automation(DevicePointerTrigger),
        # device, control, value
        cv.Optional(hid.CONF_ON_CONTROL): _automation(DeviceControlTrigger),
        # device
        cv.Optional(CONF_ON_CONNECTED): _automation(ConnectionTrigger),
        cv.Optional(CONF_ON_DISCONNECTED): _automation(ConnectionTrigger),
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

CONFIG_SCHEMA = cv.All(_BASE_SCHEMA, _generate_slots)


async def _slots_to_code(var, slots) -> None:
    from esphome.components import ble_broadcast, ble_client, ble_client_hid, bt_hid

    for conf in slots.get("bt_hid", []):
        await bt_hid.to_code(conf)
        slot = await cg.get_variable(conf[CONF_ID])
        cg.add(slot.set_log_name(""))
        cg.add(var.add_hid_slot(slot, slot))
    for conf in slots.get("ble_client", []):
        await ble_client.to_code(conf)
    for conf in slots.get("ble_client_hid", []):
        await ble_client_hid.slot_to_code(conf)
        slot = await cg.get_variable(conf[CONF_ID])
        cg.add(slot.set_log_name(""))
        cg.add(var.add_hid_slot(slot, slot))
    for conf in slots.get("ble_broadcast", []):
        await ble_broadcast.to_code(conf)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_BT_PAIRING")
    cg.add(var.set_max_devices(config[CONF_MAX_DEVICES]))
    # Bluedroid keeps the bonds itself, CONFIG_BT_SMP_MAX_BONDS of them (15 by default, at most
    # 32); beyond that it deletes one per new bond and the excess at boot (btc_storage.c).
    esp32.set_idf_sdkconfig_default("CONFIG_BT_SMP_MAX_BONDS", config[CONF_MAX_DEVICES])
    cg.add(var.set_preference_key(fnv1_hash(f"bt_pairing:{config[CONF_ID]}")))
    if CONF_FILE in config:
        cg.add_define("USE_BT_PAIRING_STORAGE")
        cg.add(var.set_file(config[CONF_FILE]))
        request_storage_worker()
    if esp32_ble_tracker.CONF_ESP32_BLE_ID in config:
        cg.add_define("USE_BT_PAIRING_LE")
        await esp32_ble_tracker.register_ble_device(var, config)
    for conf in config.get(CONF_ON_STATUS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "message")], conf)
    for key, connected in ((CONF_ON_CONNECTED, True), (CONF_ON_DISCONNECTED, False)):
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var, connected)
            await automation.build_automation(
                trigger, [(cg.std_string, "device")], conf
            )
    if config[CONF_MAX_HID_DEVICES]:
        cg.add_define("USE_BT_PAIRING_HID")
        cg.add(var.set_max_hid_devices(config[CONF_MAX_HID_DEVICES]))
        hid.setup_hid_layout(var, config[hid.CONF_LAYOUT], config[hid.CONF_NUM_LOCK])
        device = (cg.std_string, "device")
        for conf in config.get(hid.CONF_ON_KEY, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            if (key := conf.get(CONF_KEY)) is not None:
                cg.add(trigger.set_key(*key))
            await automation.build_automation(
                trigger, [device, (cg.std_string, "key"), (cg.bool_, "pressed")], conf
            )
        for conf in config.get(hid.CONF_ON_TEXT, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(
                trigger, [device, (cg.std_string, "text")], conf
            )
        for conf in config.get(hid.CONF_ON_POINTER, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(
                trigger,
                [device]
                + [(cg.int_, n) for n in ("dx", "dy", "wheel", "pan", "buttons")],
                conf,
            )
        for conf in config.get(hid.CONF_ON_CONTROL, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(
                trigger, [device, (cg.std_string, "control"), (cg.int_, "value")], conf
            )
    await _slots_to_code(var, config[CONF_SLOTS])


def _final_validate(config):
    full = fv.full_config.get()
    if CONF_FILE in config and "storage" not in full:
        raise cv.Invalid(
            f"{CONF_FILE} needs a storage (sd_storage, usb_storage, ...) mounted there"
        )
    if config[CONF_SLOTS].get("bt_hid") and not any(
        c.get("bluedroid") for c in full.get("usb_bluetooth") or []
    ):
        raise cv.Invalid(
            "Bluetooth Classic HID needs a usb_bluetooth dongle with bluedroid: true"
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


ADDRESS_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(BTPairing),
        cv.Required(CONF_ADDRESS): cv.templatable(_validate_address),
    }
)


@automation.register_action(
    "bt_pairing.pair", PairAction, ADDRESS_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "bt_pairing.connect", ConnectAction, ADDRESS_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "bt_pairing.disconnect", DisconnectAction, ADDRESS_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "bt_pairing.unpair", UnpairAction, ADDRESS_ACTION_SCHEMA, synchronous=True
)
async def address_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_address(await cg.templatable(config[CONF_ADDRESS], args, cg.std_string))
    )
    return var


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
