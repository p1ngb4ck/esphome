from esphome import automation
import esphome.codegen as cg
from esphome.components import speaker, touchscreen
from esphome.components.const import CONF_HOST
from esphome.components.esp32 import VARIANT_ESP32P4, only_on_variant, require_vfs_select
from esphome.components.lvgl.defines import CONF_LVGL_ID
from esphome.components.lvgl.hid_input import KeyBindings
from esphome.components.lvgl.video_output import (
    VIDEO_OUTPUT_SCHEMA,
    final_validate_video_output,
    video_output_to_code,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INDEX,
    CONF_NAME,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_PROTOCOL,
    CONF_SPEAKER,
)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32", "psram", "network", "lvgl"]

CONF_HID_IDS = "hid_ids"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_CONNECTIONS = "connections"
CONF_CONNECTIONS_FILE = "connections_file"
CONF_KEY_BINDINGS_ID = "key_bindings_id"

remote_desktop_ns = cg.esphome_ns.namespace("remote_desktop")
RemoteDesktop = remote_desktop_ns.class_("RemoteDesktop", cg.Component)
ConnectAction = remote_desktop_ns.class_("ConnectAction", automation.Action)
DisconnectAction = remote_desktop_ns.class_("DisconnectAction", automation.Action)
ConnectedCondition = remote_desktop_ns.class_("ConnectedCondition", automation.Condition)
ConnectSavedAction = remote_desktop_ns.class_("ConnectSavedAction", automation.Action)
SaveAction = remote_desktop_ns.class_("SaveAction", automation.Action)
ForgetAction = remote_desktop_ns.class_("ForgetAction", automation.Action)
HIDInput = cg.esphome_ns.namespace("hid").class_("HIDInput")
Protocol = RemoteDesktop.enum("Protocol")
PROTOCOLS = {"vnc": Protocol.PROTOCOL_VNC, "spice": Protocol.PROTOCOL_SPICE, "auto": Protocol.PROTOCOL_AUTO}


def _storage_path(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/"):
        raise cv.Invalid("a file's storage path, as /sd/desktops.txt")
    return value

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RemoteDesktop),
            cv.Optional(CONF_HID_IDS, default=[]): cv.All(cv.ensure_list(cv.use_id(HIDInput)), cv.Length(max=4)),
            cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(touchscreen.Touchscreen),
            cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_CONNECTIONS, default=8): cv.int_range(min=1, max=16),
            cv.Optional(CONF_CONNECTIONS_FILE): _storage_path,
            cv.Optional(CONF_KEY_BINDINGS_ID): cv.use_id(KeyBindings),
        }
    )
    .extend(VIDEO_OUTPUT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(CONF_LVGL_ID),
    only_on_variant(supported=[VARIANT_ESP32P4]),
)

FINAL_VALIDATE_SCHEMA = final_validate_video_output


async def to_code(config):
    cg.add_define("USE_REMOTE_DESKTOP")
    require_vfs_select()
    var = cg.Pvariable(config[CONF_ID], cg.MockObj(str(RemoteDesktop), "::").create())
    await cg.register_component(var, config)
    await video_output_to_code(var, config)
    for hid_id in config[CONF_HID_IDS]:
        cg.add(var.add_hid(await cg.get_variable(hid_id)))
    if CONF_TOUCHSCREEN_ID in config:
        cg.add_define("USE_REMOTE_DESKTOP_TOUCH")
        cg.add(var.set_touchscreen(await cg.get_variable(config[CONF_TOUCHSCREEN_ID])))
    if CONF_SPEAKER in config:
        cg.add_define("USE_REMOTE_DESKTOP_SPEAKER")
        cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))
    cg.add(var.set_connections(config[CONF_CONNECTIONS]))
    if CONF_CONNECTIONS_FILE in config:
        from esphome.components.storage import request_storage_worker

        cg.add_define("USE_REMOTE_DESKTOP_CONNECTIONS_FILE")
        request_storage_worker()
        cg.add(var.set_connections_file(config[CONF_CONNECTIONS_FILE]))
    if CONF_KEY_BINDINGS_ID in config:
        cg.add_define("USE_REMOTE_DESKTOP_KEY_BINDINGS")
        cg.add(var.set_key_bindings(await cg.get_variable(config[CONF_KEY_BINDINGS_ID])))


CONNECT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(RemoteDesktop),
        cv.Optional(CONF_PROTOCOL, default="vnc"): cv.one_of(*PROTOCOLS, lower=True),
        cv.Required(CONF_HOST): cv.templatable(cv.string),
        cv.Required(CONF_PORT): cv.templatable(cv.port),
        cv.Optional(CONF_PASSWORD, default=""): cv.templatable(cv.string),
    }
)


@automation.register_action("remote_desktop.connect", ConnectAction, CONNECT_SCHEMA, synchronous=True)
async def connect_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_protocol(PROTOCOLS[config[CONF_PROTOCOL]]))
    cg.add(var.set_host(await cg.templatable(config[CONF_HOST], args, cg.std_string)))
    cg.add(var.set_port(await cg.templatable(config[CONF_PORT], args, cg.uint16)))
    cg.add(var.set_password(await cg.templatable(config[CONF_PASSWORD], args, cg.std_string)))
    return var


SIMPLE_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(RemoteDesktop)})


@automation.register_action("remote_desktop.disconnect", DisconnectAction, SIMPLE_SCHEMA, synchronous=True)
async def disconnect_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("remote_desktop.is_connected", ConnectedCondition, SIMPLE_SCHEMA)
async def connected_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "remote_desktop.connect_saved",
    ConnectSavedAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(RemoteDesktop),
            cv.Required(CONF_INDEX): cv.templatable(cv.int_range(min=0, max=15)),
            cv.Optional(CONF_PASSWORD, default=""): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def connect_saved_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_index(await cg.templatable(config[CONF_INDEX], args, cg.uint8)))
    cg.add(var.set_password(await cg.templatable(config[CONF_PASSWORD], args, cg.std_string)))
    return var


@automation.register_action(
    "remote_desktop.save",
    SaveAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(RemoteDesktop),
            cv.Required(CONF_INDEX): cv.templatable(cv.int_range(min=0, max=15)),
            cv.Optional(CONF_NAME, default=""): cv.templatable(cv.string),
            cv.Required(CONF_HOST): cv.templatable(cv.string_strict),
            cv.Optional(CONF_PORT, default=5900): cv.templatable(cv.port),
        }
    ),
    synchronous=True,
)
async def save_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_index(await cg.templatable(config[CONF_INDEX], args, cg.uint8)))
    cg.add(var.set_name(await cg.templatable(config[CONF_NAME], args, cg.std_string)))
    cg.add(var.set_host(await cg.templatable(config[CONF_HOST], args, cg.std_string)))
    cg.add(var.set_port(await cg.templatable(config[CONF_PORT], args, cg.uint16)))
    return var


@automation.register_action(
    "remote_desktop.forget",
    ForgetAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(RemoteDesktop),
            cv.Required(CONF_INDEX): cv.templatable(cv.int_range(min=0, max=15)),
        }
    ),
    synchronous=True,
)
async def forget_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_index(await cg.templatable(config[CONF_INDEX], args, cg.uint8)))
    return var
