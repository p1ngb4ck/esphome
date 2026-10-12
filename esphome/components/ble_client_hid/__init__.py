"""HID over GATT: Bluetooth LE remotes, keyboards, mice and game controllers.

From fsievers22/esphome-ble-remote (MIT): pull request #45 by Gur Kohli, with the
keepalive, connection latency and fast reconnect of #47 by Attila Prokai, Jean-Noel
Goumain's connection parameter fixes and publish_to_ha of #36. Decoded reports feed the
shared hid input (keys, text, pointer, controls, layouts), as usb_hid does.
"""

import esphome.codegen as cg
from esphome.components import ble_client
from esphome.components.hid import HID_INPUT_SCHEMA, HIDInput, setup_hid_input
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

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
    "BLEClientHID", cg.Component, ble_client.BLEClientNode, HIDInput
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
    .extend(
        {
            cv.GenerateID(): cv.declare_id(BLEClientHID),
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


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
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
