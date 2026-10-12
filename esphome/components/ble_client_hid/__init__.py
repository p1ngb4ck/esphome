"""HID over GATT: Bluetooth LE remotes, keyboards, mice and game controllers.

From fsievers22/esphome-ble-remote (MIT): pull request #45 by Gur Kohli, with the
keepalive, connection latency and fast reconnect of #47 by Attila Prokai, Jean-Noel
Goumain's connection parameter fixes and publish_to_ha of #36. Decoded reports feed the
shared hid input (keys, text, pointer, controls, layouts), as usb_hid does.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker
from esphome.components.bt_pairing import CONF_BT_PAIRING_ID, BTPairing
from esphome.components.hid import HID_INPUT_SCHEMA, HIDInput, setup_hid_input
import esphome.config_validation as cv
from esphome.const import CONF_DURATION, CONF_ID, CONF_MAC_ADDRESS
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@fsievers22", "@p1ngb4ck"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["hid", "sha256"]
MULTI_CONF = True

CONF_EVENT_SAMPLING_INTERVAL = "event_sampling_interval"
CONF_DISCOVERY_MODE = "discovery_mode"
CONF_PROTOCOL_MODE = "protocol_mode"
CONF_PUBLISH_TO_HA = "publish_to_ha"
CONF_KEEPALIVE_INTERVAL = "keepalive_interval"
CONF_CONN_LATENCY = "conn_latency"
CONF_BLE_CLIENT_HID_ID = "ble_client_hid_id"

ble_client_hid_ns = cg.esphome_ns.namespace("ble_client_hid")
BLEClientHID = ble_client_hid_ns.class_(
    "BLEClientHID",
    cg.Component,
    ble_client.BLEClientNode,
    esp32_ble_tracker.ESPBTDeviceListener,
    HIDInput,
)
PairAction = ble_client_hid_ns.class_(
    "PairAction", automation.Action, cg.Parented.template(BLEClientHID)
)
UnpairAction = ble_client_hid_ns.class_(
    "UnpairAction", automation.Action, cg.Parented.template(BLEClientHID)
)
DiscoveryMode = ble_client_hid_ns.enum("DiscoveryMode", is_class=True)
ProtocolModePolicy = ble_client_hid_ns.enum("ProtocolModePolicy", is_class=True)

DISCOVERY_MODES = {
    "standard": DiscoveryMode.STANDARD,
    "forensic": DiscoveryMode.FORENSIC,
}
PROTOCOL_MODES = {
    "unchanged": ProtocolModePolicy.UNCHANGED,
    "report": ProtocolModePolicy.REPORT,
    "boot": ProtocolModePolicy.BOOT,
}


def validate_event_sampling_interval(value):
    value = cv.positive_time_period_microseconds(value)
    if value.total_microseconds > 3_600_000_000:
        raise cv.Invalid("event_sampling_interval must not exceed 1 hour")
    return value


# Keys, text, pointer and controls: see hid.HID_INPUT_SCHEMA. Keys as binary sensors or
# events and typed text are the hid platforms, given this instance's id as hid_id.
CONFIG_SCHEMA = (
    cv.COMPONENT_SCHEMA.extend(HID_INPUT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .extend(
        {
            cv.GenerateID(): cv.declare_id(BLEClientHID),
            # With a ble_client without mac_address this is a slot: ble_client_hid.pair
            # finds an advertising HID device and bt_pairing keeps it across reboots.
            cv.OnlyWith(CONF_BT_PAIRING_ID, "bt_pairing"): cv.use_id(BTPairing),
            # Coalesces relative motion (pointer, dials) in the Home Assistant events and
            # last-event sensors; keys, text and on_pointer always see every report.
            cv.Optional(
                CONF_EVENT_SAMPLING_INTERVAL, default="0ms"
            ): validate_event_sampling_interval,
            # forensic reads and subscribes everything the device offers, for diagnosis.
            cv.Optional(CONF_DISCOVERY_MODE, default="standard"): cv.enum(
                DISCOVERY_MODES, lower=True
            ),
            cv.Optional(CONF_PROTOCOL_MODE, default="unchanged"): cv.enum(
                PROTOCOL_MODES, lower=True
            ),
            # Fires esphome.hid_events in Home Assistant for every decoded value. Needs the
            # api component, and "Allow the device to perform Home Assistant actions" on
            # the device in Home Assistant.
            cv.Optional(CONF_PUBLISH_TO_HA, default=True): cv.boolean,
            # A small GATT read on the idle link every interval keeps the peripheral from
            # sleeping, so no key press is lost to a reconnect. 0s disables it; the lambda
            # set_keepalive_enabled(bool) gates it at runtime.
            cv.Optional(
                CONF_KEEPALIVE_INTERVAL, default="0s"
            ): cv.positive_time_period_milliseconds,
            # Connection events the peripheral may skip while it has nothing to send. Saves
            # its battery without delaying key presses. Unset: what the device asks for.
            cv.Optional(CONF_CONN_LATENCY): cv.int_range(min=0, max=499),
        }
    )
)

BLE_CLIENT_HID_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BLE_CLIENT_HID_ID): cv.use_id(BLEClientHID),
    }
)


def _client_config(full_config, client_id):
    for conf in full_config.get("ble_client") or []:
        if str(conf[CONF_ID]) == str(client_id):
            return conf
    return None


def _is_slot(full_config, config) -> bool:
    client = _client_config(full_config, config[ble_client.CONF_BLE_CLIENT_ID])
    return client is not None and CONF_MAC_ADDRESS not in client


def _final_validate(config):
    if _is_slot(fv.full_config.get(), config) and CONF_BT_PAIRING_ID not in config:
        raise cv.Invalid(
            "a ble_client_hid whose ble_client has no mac_address is paired at runtime and "
            "needs bt_pairing to keep the device"
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_slot(fnv1_hash(f"ble_client_hid:{config[CONF_ID]}")))
    if _is_slot(CORE.config, config):
        cg.add(var.set_dynamic(True))
        # Pairing looks for advertising HID devices.
        await esp32_ble_tracker.register_ble_device(var, config)
    if CONF_BT_PAIRING_ID in config:
        pairing = await cg.get_variable(config[CONF_BT_PAIRING_ID])
        cg.add(var.set_pairing(pairing))
    await setup_hid_input(var, config)
    cg.add(var.set_event_sampling_interval_us(config[CONF_EVENT_SAMPLING_INTERVAL]))
    cg.add(var.set_discovery_mode(config[CONF_DISCOVERY_MODE]))
    cg.add(var.set_protocol_mode_policy(config[CONF_PROTOCOL_MODE]))
    publish = config[CONF_PUBLISH_TO_HA] and "api" in CORE.loaded_integrations
    cg.add(var.set_publish_to_ha(publish))
    if publish:
        # The same define the api component sets for homeassistant.event actions.
        cg.add_define("USE_API_HOMEASSISTANT_SERVICES")
    cg.add(var.set_keepalive_interval(config[CONF_KEEPALIVE_INTERVAL]))
    if (latency := config.get(CONF_CONN_LATENCY)) is not None:
        cg.add(var.set_conn_latency(latency))


async def register_last_event_usage_text_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_last_event_usage_text_sensor(var))


async def register_last_event_code_text_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_last_event_code_text_sensor(var))


async def register_last_event_value_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_last_event_value_sensor(var))


async def register_battery_sensor(var, config):
    parent = await cg.get_variable(config[CONF_BLE_CLIENT_HID_ID])
    cg.add(parent.register_battery_sensor(var))


def _validate_pair_duration(value):
    value = cv.positive_time_period_milliseconds(value)
    if not 1000 <= value.total_milliseconds <= 300000:
        raise cv.Invalid("a pairing search lasts 1 s to 5 min")
    return value


# A slot: looks for an advertising HID device, pairs the first new one and keeps it, replacing
# the one paired before. With a configured address: connects, which pairs it.
@automation.register_action(
    "ble_client_hid.pair",
    PairAction,
    automation.maybe_simple_id(
        {
            cv.Required(CONF_ID): cv.use_id(BLEClientHID),
            cv.Optional(CONF_DURATION, default="30s"): cv.templatable(
                _validate_pair_duration
            ),
        }
    ),
    synchronous=True,
)
async def pair_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    duration = config[CONF_DURATION]
    if not cg.is_template(duration):
        duration = duration.total_milliseconds
    cg.add(var.set_duration(await cg.templatable(duration, args, cg.uint32)))
    return var


# Deletes the bond; a paired slot also forgets its device.
@automation.register_action(
    "ble_client_hid.unpair",
    UnpairAction,
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(BLEClientHID)}),
    synchronous=True,
)
async def unpair_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
