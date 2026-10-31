"""Tells a waiting receiver that the device booted again, only when one asked for it.

A receiver (a flashing tool, a test) arms it before it restarts or flashes the device (the action
boot_announce.watch, e.g. from an API action). The mark lives in RTC memory, which a restart, a crash or an
OTA reboot keep and a power cycle clears. After a boot with the mark set, once the network is up, the
device sends "<name> boot built=... reset=... uptime_ms=..." through the udp component until the receiver
answers "<name> boot ack". Sending starts on the network component's online event (active interface; with a
single interface that interface's IP event) and is resent by the scheduler only until the ack; unarmed nothing
runs.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import network
from esphome.components.udp import CONF_UDP_ID, UDPComponent
import esphome.config_validation as cv
from esphome.const import CONF_ETHERNET, CONF_ID, CONF_PRIORITY, CONF_WIFI
from esphome.core import CORE
import esphome.final_validate as fv

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["network", "udp"]

boot_announce_ns = cg.esphome_ns.namespace("boot_announce")
BootAnnounce = boot_announce_ns.class_("BootAnnounce", cg.Component)
WatchAction = boot_announce_ns.class_("WatchAction", automation.Action)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BootAnnounce),
        cv.GenerateID(CONF_UDP_ID): cv.use_id(UDPComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    # CORE.config does not exist yet here (network.has_active_interface() reads it): the same condition on the
    # final-validate view of the configuration
    full = fv.full_config.get()
    if len(full.get(network.DOMAIN, {}).get(CONF_PRIORITY, [])) <= 1 and not (
        CONF_WIFI in full or CONF_ETHERNET in full
    ):
        raise cv.Invalid(
            "boot_announce starts on the network component's online event or on the IP event of wifi or "
            "ethernet: it needs one of them"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    udp_var = await cg.get_variable(config[CONF_UDP_ID])
    cg.add(var.set_udp(udp_var))
    # The udp component creates only the sockets its users request (as udp.write and packet_transport do):
    # the send socket for the boot packets, the listen socket for the receiver's ack. Without them
    # send_packet() runs on a null socket.
    cg.add(udp_var.set_should_broadcast())
    cg.add(udp_var.set_should_listen())
    # Sending starts on the network component's online event, not by polling (checked in _final_validate)
    if await network.register_active_interface_listener(var):
        return
    if CONF_WIFI in CORE.config:
        from esphome.components import wifi

        wifi.request_wifi_ip_state_listener()
        wifi.request_wifi_connect_state_listener()
        cg.add_define("USE_BOOT_ANNOUNCE_WIFI")
    else:
        from esphome.components import ethernet

        ethernet.request_ethernet_ip_state_listener()
        cg.add_define("USE_BOOT_ANNOUNCE_ETHERNET")


@automation.register_action(
    "boot_announce.watch",
    WatchAction,
    cv.Schema({cv.GenerateID(): cv.use_id(BootAnnounce)}),
    synchronous=True,
)
async def watch_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
