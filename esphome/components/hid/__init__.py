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
from esphome.const import CONF_KEY, CONF_TRIGGER_ID
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
    }
)


def _c32(char: str) -> str:
    return f"0x{ord(char):04X}" if char else "0"


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
    rows = []
    for usage, levels in sorted(keys.items()):
        chars, dead = [], 0
        for level, symbol in enumerate(levels[: layouts.LEVELS]):
            mark = layouts.combining(symbol)
            if mark is not None:
                dead |= 1 << level
                chars.append(_c32(mark))
            else:
                chars.append(_c32(symbol if len(symbol) == 1 else ""))
        lower, upper = levels[0], levels[1] if len(levels) > 1 else ""
        alphabetic = (
            len(lower) == 1
            and lower.isalpha()
            and lower != upper
            and lower.upper() == upper
        )
        rows.append(
            f"  {{0x{usage:02X}, 0x{dead:X}, {'true' if alphabetic else 'false'}, "
            f"{{{', '.join(chars)}}}}}"
        )
    compose = [
        f"  {{{_c32(mark)}, {_c32(base)}, {_c32(result)}}}"
        for mark, base, result in layouts.compose_table(keys)
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


async def setup_hid_input(var, config) -> None:
    """Layout, Num Lock and automations of a transport configured with HID_INPUT_SCHEMA."""
    _emit_key_names()
    layout = _emit_layout(config[CONF_LAYOUT])
    cg.add(var.set_layout(cg.RawExpression(f"&esphome::hid::{layout}")))
    cg.add(var.set_num_lock(config[CONF_NUM_LOCK]))
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
