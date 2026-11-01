from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import socket, speaker, touchscreen
from esphome.components.const import CONF_HOST
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    add_idf_component,
    add_idf_sdkconfig_option,
    only_on_variant,
    require_vfs_select,
)
from esphome.components.lvgl.defines import CONF_LVGL_ID
from esphome.components.lvgl.hid_input import KeyBindings
from esphome.components.lvgl.video_output import (
    VIDEO_OUTPUT_SCHEMA,
    final_validate_video_output,
    screen_size,
    video_output_to_code,
)
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import (
    CONF_CHANNEL,
    CONF_CLOSE_ACTION,
    CONF_ID,
    CONF_INDEX,
    CONF_NUM_CHANNELS,
    CONF_PATH,
    CONF_PORT,
    CONF_ROTATION,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)
import esphome.final_validate as fv

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32", "psram", "lvgl", "storage"]
AUTO_LOAD = ["hid"]

CONF_SYSTEMS = "systems"
CONF_KEYS = "keys"
CONF_MENU_KEYS = "menu_keys"
CONF_MIN_SCALE = "min_scale"
CONF_ON_STARTED = "on_started"
CONF_ON_PAUSED = "on_paused"
CONF_ON_ENDED = "on_ended"
CONF_BIOS_DIR = "bios_dir"
CONF_HID_IDS = "hid_ids"
CONF_ON_PLAYERS_CHANGED = "on_players_changed"
CONF_ON_PLAYERS_CLOSED = "on_players_closed"
CONF_LINK = "link"
CONF_PEERS = "peers"
CONF_SLOT = "slot"
CONF_SEAT = "seat"
CONF_AUDIO_RATE = "audio_rate"
CONF_AUDIO_CHANNELS = "audio_channels"
CONF_KEY_BINDINGS_ID = "key_bindings_id"
CONF_TOUCHSCREEN_ID = "touchscreen_id"

emulator_ns = cg.esphome_ns.namespace("emulator")
Emulator = emulator_ns.class_("Emulator", cg.Component)
EmuSystem = emulator_ns.enum("EmuSystem", is_class=True)
StartAction = emulator_ns.class_("StartAction", automation.Action)
StateAction = emulator_ns.class_("StateAction", automation.Action)
PlayersAction = emulator_ns.class_("PlayersAction", automation.Action)
PlayerSetAction = emulator_ns.class_("PlayerSetAction", automation.Action)
LinkAction = emulator_ns.class_("LinkAction", automation.Action)
CommandAction = emulator_ns.class_("CommandAction", automation.Action)
HIDInput = cg.esphome_ns.namespace("hid").class_("HIDInput")

EMU_SYSTEMS = {
    "nes": EmuSystem.NES,
    "gb": EmuSystem.GB,
    "sms": EmuSystem.SMS,
    "pce": EmuSystem.PCE,
    "lynx": EmuSystem.LYNX,
    "snes": EmuSystem.SNES,
    "md": EmuSystem.MD,
    "gw": EmuSystem.GW,
    "msx": EmuSystem.MSX,
}
EMU_NATIVE_SIZES = {
    "nes": (256, 240),
    "gb": (160, 144),
    "sms": (256, 192),
    "pce": (256, 240),
    "lynx": (160, 102),
    "snes": (256, 224),
    "md": (320, 224),
    "gw": (320, 240),
    "msx": (256, 228),
}
EMU_BUTTONS = {
    "up": 1 << 0,
    "down": 1 << 1,
    "left": 1 << 2,
    "right": 1 << 3,
    "a": 1 << 4,
    "b": 1 << 5,
    "x": 1 << 6,
    "y": 1 << 7,
    "l": 1 << 8,
    "r": 1 << 9,
    "start": 1 << 10,
    "select": 1 << 11,
}
EMU_DEFAULT_KEYS = {
    "up": ["0x01:0x90", "UP"],
    "down": ["0x01:0x91", "DOWN"],
    "left": ["0x01:0x93", "LEFT"],
    "right": ["0x01:0x92", "RIGHT"],
    "a": ["BUTTON_2", "X"],
    "b": ["BUTTON_1", "Z"],
    "x": ["BUTTON_4", "S"],
    "y": ["BUTTON_3", "A"],
    "l": ["BUTTON_5", "Q"],
    "r": ["BUTTON_6", "W"],
    "start": ["BUTTON_8", "ENTER"],
    "select": ["BUTTON_7", "RIGHT_SHIFT"],
}


def _emu_key(value):
    from esphome.components.hid.keys import validate_key

    return validate_key(value)


def _storage_dir(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/"):
        raise cv.Invalid("a directory's storage path, as /sd/games/nes")
    return value


def _emu_systems(value):
    value = cv.Schema({cv.Optional(name): _storage_dir for name in EMU_SYSTEMS})(value)
    if not value:
        raise cv.Invalid(f"list at least one of {', '.join(EMU_SYSTEMS)} with its ROM directory")
    return value


def _min_scale(value):
    value = cv.float_range(min=1 / 16, max=16)(value)
    return max(1, round(value * 16))


def _emu_bios(config):
    if "msx" in config[CONF_SYSTEMS] and CONF_BIOS_DIR not in config:
        raise cv.Invalid(f"msx needs its BIOS files (MSX2.ROM, MSX2EXT.ROM, ...): set {CONF_BIOS_DIR}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Emulator),
            cv.Required(CONF_SYSTEMS): _emu_systems,
            cv.Optional(CONF_MIN_SCALE, default=1.0): _min_scale,
            cv.Optional(CONF_BIOS_DIR): _storage_dir,
            cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_KEY_BINDINGS_ID): cv.use_id(KeyBindings),
            cv.Optional(CONF_CLOSE_ACTION, default="home"): cv.string_strict,
            cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(touchscreen.Touchscreen),
            cv.Optional(CONF_HID_IDS, default=[]): cv.ensure_list(cv.use_id(HIDInput)),
            cv.Optional(CONF_KEYS, default={}): cv.Schema(
                {cv.Optional(button): cv.ensure_list(_emu_key) for button in EMU_BUTTONS}
            ),
            cv.Optional(CONF_MENU_KEYS, default=["BUTTON_11"]): cv.ensure_list(_emu_key),
            cv.Optional(CONF_ON_STARTED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_PAUSED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_ENDED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_PLAYERS_CHANGED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_PLAYERS_CLOSED): automation.validate_automation(single=True),
            cv.Optional(CONF_LINK): cv.All(
                cv.Schema(
                    {
                        cv.Optional(CONF_PORT, default=6666): cv.port,
                        cv.Optional(CONF_PEERS, default=[]): cv.ensure_list(cv.string_strict),
                    }
                ),
                socket.consume_sockets(1, "emulator"),
                socket.consume_sockets(1, "emulator", socket.SocketType.TCP_LISTEN),
            ),
        }
    )
    .extend(VIDEO_OUTPUT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(CONF_LVGL_ID),
    only_on_variant(supported=[VARIANT_ESP32P4]),
    _emu_bios,
)


def _final_validate(config):
    config = final_validate_video_output(config)
    fconf = fv.full_config.get()
    lvgl = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_LVGL_ID])[:-1])
    width, height = screen_size(config[CONF_LVGL_ID])
    if int(lvgl.get(CONF_ROTATION, 0)) in (90, 270):
        width &= ~15
    else:
        height &= ~15
    scale = config[CONF_MIN_SCALE]
    for name in config[CONF_SYSTEMS]:
        native_w, native_h = EMU_NATIVE_SIZES[name]
        if min(width * 16 // native_w, height * 16 // native_h) < scale:
            need_w = -(-native_w * scale // 16)
            need_h = -(-native_h * scale // 16)
            raise cv.Invalid(
                f"{CONF_SYSTEMS}: {name} needs a screen of at least {need_w} x {need_h} "
                f"({native_w} x {native_h} at {CONF_MIN_SCALE} {scale / 16:g}); this screen has {width} x {height}"
            )
    own = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_ID])[:-1])
    rate, channels = 48000, 2
    if CONF_SPEAKER in config:
        spk = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_SPEAKER])[:-1])
        if CONF_SAMPLE_RATE in spk:
            rate = min(48000, max(8000, int(spk[CONF_SAMPLE_RATE])))
        if CONF_NUM_CHANNELS in spk:
            channels = min(2, int(spk[CONF_NUM_CHANNELS]))
        elif CONF_CHANNEL in spk:
            channels = 2 if spk[CONF_CHANNEL] == "stereo" else 1
    own[CONF_AUDIO_RATE] = rate
    own[CONF_AUDIO_CHANNELS] = channels
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    cg.add_define("USE_EMULATOR")
    request_storage_worker()
    add_idf_sdkconfig_option("CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY", True)
    add_idf_component(name="emu", override_path=str(Path(__file__).parent / "emu"))
    var = cg.Pvariable(config[CONF_ID], cg.MockObj(str(Emulator), "::").create())
    await cg.register_component(var, config)
    await video_output_to_code(var, config)
    cg.add(var.set_lvgl(await cg.get_variable(config[CONF_LVGL_ID])))
    cg.add(var.set_audio(config.get(CONF_AUDIO_RATE, 48000), config.get(CONF_AUDIO_CHANNELS, 2)))
    if CONF_SPEAKER in config:
        cg.add_define("USE_EMULATOR_SPEAKER")
        cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))
    if CONF_KEY_BINDINGS_ID in config:
        cg.add_define("USE_EMULATOR_KEY_BINDINGS")
        cg.add(var.set_key_bindings(await cg.get_variable(config[CONF_KEY_BINDINGS_ID])))
        cg.add(var.set_close_action_name(config[CONF_CLOSE_ACTION]))
    if CONF_TOUCHSCREEN_ID in config:
        cg.add_define("USE_EMULATOR_TOUCH")
        cg.add(var.set_touchscreen(await cg.get_variable(config[CONF_TOUCHSCREEN_ID])))
    if CONF_BIOS_DIR in config:
        cg.add(var.set_bios_dir(config[CONF_BIOS_DIR]))
    for name, directory in config[CONF_SYSTEMS].items():
        cg.add_define(f"USE_EMULATOR_{name.upper()}")
        add_idf_sdkconfig_option(f"CONFIG_EMU_{name.upper()}", True)
        cg.add(var.add_system(EMU_SYSTEMS[name], directory, config[CONF_MIN_SCALE]))
    for button, bit in EMU_BUTTONS.items():
        keys = config[CONF_KEYS].get(button)
        if keys is None:
            keys = [_emu_key(key) for key in EMU_DEFAULT_KEYS[button]]
        for page, usage in keys:
            cg.add(var.add_key(page, usage, bit))
    for page, usage in config[CONF_MENU_KEYS]:
        cg.add(var.add_menu_key(page, usage))
    if on_started := config.get(CONF_ON_STARTED):
        await automation.build_automation(var.get_started_trigger(), [], on_started)
    if on_paused := config.get(CONF_ON_PAUSED):
        await automation.build_automation(var.get_paused_trigger(), [], on_paused)
    if on_ended := config.get(CONF_ON_ENDED):
        await automation.build_automation(var.get_ended_trigger(), [], on_ended)
    for hid_id in config[CONF_HID_IDS]:
        cg.add(var.add_hid(await cg.get_variable(hid_id)))
    if on_changed := config.get(CONF_ON_PLAYERS_CHANGED):
        await automation.build_automation(var.get_players_changed_trigger(), [], on_changed)
    if on_closed := config.get(CONF_ON_PLAYERS_CLOSED):
        await automation.build_automation(var.get_players_closed_trigger(), [], on_closed)
    if link := config.get(CONF_LINK):
        cg.add_define("USE_EMULATOR_LINK")
        require_vfs_select()
        cg.add(var.set_link_port(link[CONF_PORT]))
        for peer in link[CONF_PEERS]:
            cg.add(var.add_peer(peer))


EMULATOR_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(Emulator)})


@automation.register_action(
    "emulator.start",
    StartAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(Emulator),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def emulator_start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


EMULATOR_COMMANDS = {"stop": 0, "pause": 1, "resume": 2, "players_start": 3, "players_cancel": 4}


def _register_command(name, command):
    @automation.register_action(f"emulator.{name}", CommandAction, EMULATOR_ACTION_SCHEMA, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_command(command))
        return var

    return _to_code


for _name, _command in EMULATOR_COMMANDS.items():
    _register_command(_name, _command)


def _register_state(name, save):
    @automation.register_action(
        f"emulator.{name}",
        StateAction,
        cv.Schema(
            {
                cv.GenerateID(): cv.use_id(Emulator),
                cv.Required(CONF_SLOT): cv.templatable(cv.int_range(min=1, max=4)),
            }
        ),
        synchronous=True,
    )
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_slot(await cg.templatable(config[CONF_SLOT], args, cg.uint8)))
        cg.add(var.set_save(save))
        return var

    return _to_code


_register_state("save_state", True)
_register_state("load_state", False)


@automation.register_action(
    "emulator.players",
    PlayersAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(Emulator),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def emulator_players_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


@automation.register_action(
    "emulator.players_set",
    PlayerSetAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(Emulator),
            cv.Required(CONF_INDEX): cv.templatable(cv.int_range(min=0, max=3)),
            cv.Required(CONF_SEAT): cv.templatable(cv.int_range(min=0, max=2)),
        }
    ),
    synchronous=True,
)
async def emulator_players_set_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_index(await cg.templatable(config[CONF_INDEX], args, cg.uint8)))
    cg.add(var.set_seat(await cg.templatable(config[CONF_SEAT], args, cg.uint8)))
    return var


def _register_link(name, mode):
    schema = EMULATOR_ACTION_SCHEMA
    if mode == 2:
        schema = cv.Schema(
            {
                cv.GenerateID(): cv.use_id(Emulator),
                cv.Required(CONF_HOST): cv.templatable(cv.string_strict),
            }
        )

    @automation.register_action(f"emulator.{name}", LinkAction, schema, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_mode(mode))
        if mode == 2:
            cg.add(var.set_host(await cg.templatable(config[CONF_HOST], args, cg.std_string)))
        return var

    return _to_code


_register_link("link_off", 0)
_register_link("link_host", 1)
_register_link("link_join", 2)
