"""Shared HID input: report descriptor parsing, keyboard layouts, keys, text and pointer.

Not configured on its own. A transport (usb_hid, ble_client_hid, bt_hid) derives its
component from HIDInput, extends its schema with HID_INPUT_SCHEMA and calls
setup_hid_input() from its to_code().

The report descriptor parser and the usage tables come from fsievers22/esphome-ble-remote
(MIT), pull request #45 by Gur Kohli.
"""

from __future__ import annotations

from esphome import automation
import esphome.codegen as cg
from esphome.components import key_provider
import esphome.config_validation as cv
from esphome.const import (
    CONF_BLUE,
    CONF_DURATION,
    CONF_GREEN,
    CONF_ID,
    CONF_KEY,
    CONF_RED,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE

from . import layouts
from .keys import KEY_NAMES, validate_key

CODEOWNERS = ["@p1ngb4ck"]
AUTO_LOAD = ["key_provider"]

CONF_HID_ID = "hid_id"
CONF_LAYOUT = "layout"
CONF_BASE = "base"
CONF_KEYS = "keys"
CONF_NUM_LOCK = "num_lock"
CONF_ON_KEY = "on_key"
CONF_ON_TEXT = "on_text"
CONF_ON_POINTER = "on_pointer"
CONF_ON_CONTROL = "on_control"
CONF_API_ACTIONS = "api_actions"
CONF_STRONG = "strong"
CONF_WEAK = "weak"
CONF_PLAYER = "player"
CONF_API_ACTIONS_ID = "api_actions_id"

hid_ns = cg.esphome_ns.namespace("hid")
HIDInput = hid_ns.class_("HIDInput", key_provider.KeyProvider)
KeyTrigger = hid_ns.class_(
    "KeyTrigger", automation.Trigger.template(cg.std_string, cg.bool_)
)
TextTrigger = hid_ns.class_("TextTrigger", automation.Trigger.template(cg.std_string))
PointerTrigger = hid_ns.class_(
    "PointerTrigger",
    automation.Trigger.template(cg.int_, cg.int_, cg.int_, cg.int_, cg.int_),
)
ControlTrigger = hid_ns.class_(
    "ControlTrigger", automation.Trigger.template(cg.std_string, cg.int_)
)
HIDAPIActions = hid_ns.class_("HIDAPIActions", cg.Component)
RumbleAction = hid_ns.class_("RumbleAction", automation.Action)
SetPlayerLedAction = hid_ns.class_("SetPlayerLedAction", automation.Action)
SetLightBarAction = hid_ns.class_("SetLightBarAction", automation.Action)

# Nothing to configure here; it lets the C++ API be used without a transport (tests).
CONFIG_SCHEMA = cv.Schema({})

DOMAIN = "hid"
KEY_EMITTED = "emitted"


def _validate_layout_name(value):
    value = cv.string_strict(value)
    try:
        layouts.resolve(value)
    except KeyError as err:
        raise cv.Invalid(
            f"Unknown keyboard layout '{value}'. Layouts are named as in XKB: a country "
            "file such as 'de', or one of its variants such as 'de(nodeadkeys)'"
        ) from err
    return value


def _validate_levels(value):
    value = cv.ensure_list(cv.string)(value)
    if not 1 <= len(value) <= layouts.LEVELS:
        raise cv.Invalid(
            "a key lists what it types plain, with Shift, with AltGr and with "
            "Shift+AltGr: one to four entries"
        )
    for entry in value:
        if len(entry) > 1 and layouts.combining(entry) is None:
            raise cv.Invalid(
                f"'{entry}' is neither one character nor a dead key such as dead_acute"
            )
    return value


def _validate_layout_key(value):
    page, usage = validate_key(value)
    if page != 0x07:
        raise cv.Invalid(f"{value} is not a keyboard key")
    return usage


# A layout by name, or one based on another with keys of its own:
#   layout: de(nodeadkeys)
#   layout:
#     base: de
#     keys:
#       NON_US_BACKSLASH: ["<", ">", "|"]
LAYOUT_SCHEMA = cv.Any(
    _validate_layout_name,
    cv.Schema(
        {
            cv.Optional(CONF_BASE, default="us"): _validate_layout_name,
            cv.Optional(CONF_KEYS, default={}): cv.Schema(
                {_validate_layout_key: _validate_levels}
            ),
        }
    ),
)

HID_INPUT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_LAYOUT, default="us"): LAYOUT_SCHEMA,
        cv.Optional(CONF_NUM_LOCK, default=True): cv.boolean,
        cv.Optional(CONF_ON_KEY): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(KeyTrigger),
                cv.Optional(CONF_KEY): validate_key,
            }
        ),
        cv.Optional(CONF_ON_TEXT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TextTrigger)}
        ),
        cv.Optional(CONF_ON_POINTER): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PointerTrigger)}
        ),
        cv.Optional(CONF_ON_CONTROL): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ControlTrigger)}
        ),
        # The pad outputs (hid.rumble, hid.set_player_led, hid.set_light_bar) as Home
        # Assistant actions named after this id: <id>_rumble(strong, weak, duration_ms),
        # <id>_set_player_led(player), <id>_set_light_bar(red, green, blue).
        cv.Optional(CONF_API_ACTIONS, default=False): cv.boolean,
        cv.GenerateID(CONF_API_ACTIONS_ID): cv.declare_id(HIDAPIActions),
    }
)


def _c32(code: int) -> str:
    return f"0x{code:04X}" if code else "0"


def _emit_key_names() -> None:
    data = CORE.data.setdefault(DOMAIN, {})
    if data.get(KEY_EMITTED):
        return
    data[KEY_EMITTED] = True
    entries = ",\n".join(
        f'  {{0x{page:02X}, 0x{usage:02X}, "{name}"}}'
        for name, (page, usage) in sorted(KEY_NAMES.items(), key=lambda x: x[1])
    )
    cg.add_global(
        cg.RawStatement(
            "namespace esphome::hid {\n"
            "extern const KeyName KEY_NAMES[];\n"
            f"const KeyName KEY_NAMES[] = {{\n{entries}\n}};\n"
            f"const uint16_t KEY_NAMES_COUNT = {len(KEY_NAMES)};\n"
            "}  // namespace esphome::hid"
        )
    )


def _layout_keys(config) -> tuple[str, dict[int, tuple[str, ...]]]:
    if isinstance(config, str):
        return config, layouts.resolve(config)[1]
    _, keys = layouts.resolve(config[CONF_BASE])
    keys = dict(keys)
    for usage, levels in config[CONF_KEYS].items():
        keys[usage] = tuple(levels) + ("",) * (layouts.LEVELS - len(levels))
    return f"{config[CONF_BASE]}+{sorted(config[CONF_KEYS].items())}", keys


def _emit_layout(config) -> str:
    """The C++ name of the layout's table, emitted once per distinct layout."""
    key, keys = _layout_keys(config)
    emitted = CORE.data.setdefault(DOMAIN, {}).setdefault("layouts", {})
    if key in emitted:
        return emitted[key]
    name = f"HID_LAYOUT_{len(emitted)}"
    emitted[key] = name
    keys_table, compose_table = layouts.table(keys)
    rows = [
        f"  {{0x{usage:02X}, 0x{dead:X}, {'true' if alphabetic else 'false'}, "
        f"{{{', '.join(_c32(c) for c in chars)}}}}}"
        for usage, dead, alphabetic, chars in keys_table
    ]
    compose = [
        f"  {{{_c32(mark)}, {_c32(base)}, {_c32(result)}}}"
        for mark, base, result in compose_table
    ]
    compose_body = ",\n".join(compose) if compose else "  {0, 0, 0}"
    cg.add_global(
        cg.RawStatement(
            "namespace esphome::hid {\n"
            f"static const LayoutKey {name}_KEYS[] = {{\n" + ",\n".join(rows) + "\n};\n"
            f"static const LayoutCompose {name}_COMPOSE[] = {{\n{compose_body}\n}};\n"
            f"static const KeyboardLayout {name} = {{{name}_KEYS, {len(rows)}, "
            f"{name}_COMPOSE, {len(compose)}}};\n"
            "}  // namespace esphome::hid"
        )
    )
    return name


def setup_hid_layout(var, layout_config, num_lock: bool) -> None:
    """Layout and Num Lock of a HIDInput."""
    _emit_key_names()
    layout = _emit_layout(layout_config)
    cg.add(var.set_layout(cg.RawExpression(f"&esphome::hid::{layout}")))
    cg.add(var.set_num_lock(num_lock))


async def setup_hid_input(var, config) -> None:
    """Layout, Num Lock and automations of a transport configured with HID_INPUT_SCHEMA."""
    setup_hid_layout(var, config[CONF_LAYOUT], config[CONF_NUM_LOCK])
    cg.add(var.set_log_name(str(config[CONF_ID])))
    for conf in config.get(CONF_ON_KEY, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        if (key := conf.get(CONF_KEY)) is not None:
            cg.add(trigger.set_key(*key))
        await automation.build_automation(
            trigger, [(cg.std_string, "key"), (cg.bool_, "pressed")], conf
        )
    for conf in config.get(CONF_ON_TEXT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "text")], conf)
    for conf in config.get(CONF_ON_POINTER, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [
                (cg.int_, "dx"),
                (cg.int_, "dy"),
                (cg.int_, "wheel"),
                (cg.int_, "pan"),
                (cg.int_, "buttons"),
            ],
            conf,
        )
    for conf in config.get(CONF_ON_CONTROL, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "control"), (cg.int_, "value")], conf
        )
    if config.get(CONF_API_ACTIONS, False):
        if "api" not in CORE.config:
            raise cv.Invalid(f"{CONF_API_ACTIONS} needs the api component")
        cg.add_define("USE_HID_API_ACTIONS")
        cg.add_define("USE_API_USER_DEFINED_ACTIONS")
        cg.add_define("USE_API_CUSTOM_SERVICES")
        actions = cg.new_Pvariable(
            config[CONF_API_ACTIONS_ID], var, str(config[CONF_ID])
        )
        await cg.register_component(actions, {})


# Pad outputs. Values are fractions (percentages); a pad without that output ignores it.
@automation.register_action(
    "hid.rumble",
    RumbleAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HIDInput),
            cv.Optional(CONF_STRONG, default=1.0): cv.templatable(cv.percentage),
            cv.Optional(CONF_WEAK, default=1.0): cv.templatable(cv.percentage),
            # 0: until the next hid.rumble (strong and weak 0 stop it).
            cv.Optional(CONF_DURATION, default="0ms"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
    synchronous=True,
)
async def rumble_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    cg.add(var.set_strong(await cg.templatable(config[CONF_STRONG], args, cg.float_)))
    cg.add(var.set_weak(await cg.templatable(config[CONF_WEAK], args, cg.float_)))
    duration = config[CONF_DURATION]
    if not cg.is_template(duration):
        duration = duration.total_milliseconds
    cg.add(var.set_duration(await cg.templatable(duration, args, cg.uint32)))
    return var


@automation.register_action(
    "hid.set_player_led",
    SetPlayerLedAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HIDInput),
            # 0 off; the pads have 4 (DualSense 5).
            cv.Required(CONF_PLAYER): cv.templatable(cv.int_range(min=0, max=5)),
        }
    ),
    synchronous=True,
)
async def set_player_led_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    cg.add(var.set_player(await cg.templatable(config[CONF_PLAYER], args, cg.uint8)))
    return var


@automation.register_action(
    "hid.set_light_bar",
    SetLightBarAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HIDInput),
            cv.Required(CONF_RED): cv.templatable(cv.percentage),
            cv.Required(CONF_GREEN): cv.templatable(cv.percentage),
            cv.Required(CONF_BLUE): cv.templatable(cv.percentage),
        }
    ),
    synchronous=True,
)
async def set_light_bar_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    for key, setter in (
        (CONF_RED, var.set_red),
        (CONF_GREEN, var.set_green),
        (CONF_BLUE, var.set_blue),
    ):
        cg.add(setter(await cg.templatable(config[key], args, cg.float_)))
    return var
