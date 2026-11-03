import esphome.codegen as cg
from esphome.components import select, usb_host
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
import esphome.final_validate as fv

from .. import RemoteDesktop, remote_desktop_ns

DEPENDENCIES = ["remote_desktop", "usb_host"]

CONF_REMOTE_DESKTOP_ID = "remote_desktop_id"
MAX_SLOTS = 4

UsbSlot = remote_desktop_ns.class_("UsbSlot", select.Select, cg.Component)

CONFIG_SCHEMA = (
    select.select_schema(UsbSlot)
    .extend({cv.GenerateID(CONF_REMOTE_DESKTOP_ID): cv.use_id(RemoteDesktop)})
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config):
    fconf = fv.full_config.get()
    slots = [block for block in fconf.get("select") or [] if block.get("platform") == "remote_desktop"]
    if len(slots) > MAX_SLOTS:
        raise cv.Invalid(f"At most {MAX_SLOTS} USB redirection slots.")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = await select.new_select(config, options=["None"])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_REMOTE_DESKTOP_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.add_usb_slot(var))
    cg.add_define("USE_REMOTE_DESKTOP_USB")
    usb_host.require_bulk_transfers()
    usb_host.require_control_transfers()
    add_idf_sdkconfig_option("CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM", True)
