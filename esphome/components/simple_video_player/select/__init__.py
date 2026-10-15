import esphome.codegen as cg
from esphome.components import select, usb_host
import esphome.config_validation as cv
import esphome.final_validate as fv

from .. import CONF_SPICE, SimpleVideoPlayer, simple_video_player_ns

DEPENDENCIES = ["simple_video_player", "usb_host"]

CONF_SIMPLE_VIDEO_PLAYER_ID = "simple_video_player_id"
MAX_SLOTS = 4  # SimpleVideoPlayer::SPICE_USB_SLOTS

SpiceUsbSlot = simple_video_player_ns.class_("SpiceUsbSlot", select.Select, cg.Component)

# A USB redirection slot of the SPICE client: the n-th one takes the server's usbredir channel n.
CONFIG_SCHEMA = (
    select.select_schema(SpiceUsbSlot)
    .extend({cv.GenerateID(CONF_SIMPLE_VIDEO_PLAYER_ID): cv.use_id(SimpleVideoPlayer)})
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config):
    fconf = fv.full_config.get()
    player = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_SIMPLE_VIDEO_PLAYER_ID])[:-1])
    if not player.get(CONF_SPICE):
        raise cv.Invalid("A SPICE USB redirection slot needs `spice: true` on the simple_video_player.")
    slots = [
        block
        for block in fconf.get("select") or []
        if block.get("platform") == "simple_video_player"
    ]
    if len(slots) > MAX_SLOTS:
        raise cv.Invalid(f"At most {MAX_SLOTS} SPICE USB redirection slots.")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    # The options are the devices plugged in, filled in at runtime.
    var = await select.new_select(config, options=["None"])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_SIMPLE_VIDEO_PLAYER_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.add_spice_usb_slot(var))
    cg.add_define("SVP_SPICE_USB")
    usb_host.require_bulk_transfers()
    usb_host.require_control_transfers()
