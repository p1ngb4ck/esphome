"""Tells a waiting receiver that the device booted again, only when one asked for it.

A receiver (a flashing tool, a test) arms it before it restarts or flashes the device (the action
boot_announce.watch, e.g. from an API action). The mark lives in RTC memory, which a restart, a crash or an
OTA reboot keep and a power cycle clears. After a boot with the mark set, once the network is up, the
device sends "<name> boot built=... reset=... uptime_ms=..." through the udp component until the receiver
answers "<name> boot ack"; then the mark is cleared. Unarmed it does nothing (its loop stays off).
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components.udp import CONF_UDP_ID, UDPComponent
import esphome.config_validation as cv
from esphome.const import CONF_ID

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


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_udp(await cg.get_variable(config[CONF_UDP_ID])))


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
