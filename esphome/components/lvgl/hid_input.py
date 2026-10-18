"""HID keyboards, remotes and mice (usb_hid, ble_client_hid, bt_hid) as LVGL input devices."""

import re

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTION,
    CONF_ACTIONS,
    CONF_AUTOMATION_ID,
    CONF_FILE,
    CONF_GROUP,
    CONF_ID,
    CONF_KEY,
    CONF_NAME,
    CONF_THEN,
    CONF_TIMEOUT,
    CONF_TRIGGER_ID,
)
from esphome.helpers import fnv1_hash

from .defines import CONF_LONG_PRESS_REPEAT_TIME, CONF_LONG_PRESS_TIME, add_lv_use
from .lv_validation import lv_image
from .lvcode import lv, lv_assign, lv_expr, lv_Pvariable
from .schemas import PRESS_TIME
from .types import lv_group_t, lvgl_ns

CONF_HID_ID = "hid_id"
CONF_HID_KEYBOARDS = "hid_keyboards"
CONF_HID_POINTERS = "hid_pointers"
CONF_CURSOR = "cursor"
CONF_HIDE_AFTER = "hide_after"
CONF_KEY_BINDINGS = "key_bindings"
CONF_KEYS = "keys"
CONF_ON_LEARNED = "on_learned"

LVHIDKeypadListener = lvgl_ns.class_("LVHIDKeypadListener")
LVHIDPointerListener = lvgl_ns.class_("LVHIDPointerListener")
KeyBindings = lvgl_ns.class_("KeyBindings", cg.Component)
KeyBindingsKey = KeyBindings.struct("Key")

# The built-in actions, in KeyBindings::Builtin order, with the keys they have unless the
# configuration lists others: keyboards, remotes (consumer menu keys) and game pads (D-pad).
BUILTIN_ACTIONS = {
    "up": ["UP", "MENU_UP", "0x01:0x90"],
    "down": ["DOWN", "MENU_DOWN", "0x01:0x91"],
    "left": ["LEFT", "MENU_LEFT", "0x01:0x93"],
    "right": ["RIGHT", "MENU_RIGHT", "0x01:0x92"],
    "next": ["TAB"],
    "prev": ["SHIFT+TAB"],
    "enter": ["ENTER", "KP_ENTER", "SELECT"],
    "back": ["ESCAPE", "MENU_ESCAPE", "AC_BACK", "0x07:0xF1"],  # 0x07:0xF1: Fire TV remote
    "home": ["HOME", "AC_HOME"],
    "end": ["END"],
    "page_up": ["PAGE_UP"],
    "page_down": ["PAGE_DOWN"],
    "backspace": ["BACKSPACE"],
    "delete": ["DELETE"],
}
MODIFIERS = {"SHIFT": 1, "CTRL": 2, "ALT": 4, "GUI": 8, "META": 8}


def _hid_input():
    # Imported late: hid is only loaded where a HID transport is configured.
    from esphome.components.hid import HIDInput

    return cv.use_id(HIDInput)


def _hid_id(value):
    return _hid_input()(value)


COMBO_MAX = 3  # KeyBindings::COMBO_MAX


def validate_key_combo(value):
    """A key as hid names it, with modifiers in front: "SHIFT+TAB"; or up to three keys held
    together: "BUTTON_5+BUTTON_6+BUTTON_8". (page, usage, mods, (page << 16 | usage of the others))."""
    from esphome.components.hid.keys import validate_key

    if isinstance(value, int):
        return (*validate_key(value), 0, (0, 0))
    parts = [part.strip() for part in cv.string_strict(value).strip().split("+")]
    mods = 0
    while len(parts) > 1 and parts[0].upper() in MODIFIERS:
        mods |= MODIFIERS[parts.pop(0).upper()]
    if len(parts) > COMBO_MAX:
        raise cv.Invalid(f"a combo is at most {COMBO_MAX} keys")
    keys = []
    for part in parts:
        if not part:
            raise cv.Invalid(f"Empty key in '{value}'")
        if part.upper() in MODIFIERS:
            raise cv.Invalid(f"Modifier '{part}' must come first: use {', '.join(MODIFIERS)}")
        keys.append(validate_key(part))
    others = [page << 16 | usage for page, usage in keys[1:]]
    others += [0] * (COMBO_MAX - 1 - len(others))
    return (*keys[0], mods, tuple(others))


def _action_name(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[a-z][a-z0-9_]*", value):
        raise cv.Invalid("an action name is lower case letters, digits and _")
    return value


def _unique_names(actions):
    names = [action[CONF_NAME] for action in actions]
    for name in names:
        if names.count(name) > 1:
            raise cv.Invalid(f"action '{name}' is listed twice")
    return actions


def _validate_file(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/") or value.count("/") < 2:
        raise cv.Invalid(
            "file is a path under a storage's mount_path, e.g. /sdcard/key_bindings.txt"
        )
    return value


# What the keys of the hid_keyboards do. The built-in actions (BUILTIN_ACTIONS) can be given other
# keys and an automation of their own; further actions take keys and run their automation.
KEY_BINDINGS_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(KeyBindings),
        cv.Optional(CONF_FILE): _validate_file,
        cv.Optional(CONF_ACTIONS, default=[]): cv.All(
            cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required(CONF_NAME): _action_name,
                        cv.Optional(CONF_KEYS): cv.ensure_list(validate_key_combo),
                        cv.Optional(CONF_THEN): automation.validate_action_list,
                        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                            automation.Trigger.template()
                        ),
                        cv.GenerateID(CONF_AUTOMATION_ID): cv.declare_id(
                            automation.Automation
                        ),
                    }
                )
            ),
            _unique_names,
        ),
        cv.Optional(CONF_ON_LEARNED): automation.validate_automation(single=True),
    }
)

# Navigation keys as LV_KEY_* and typed text as characters, into a group like a keypad.
HID_KEYBOARDS_CONFIG = cv.ensure_list(
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.declare_id(LVHIDKeypadListener),
            cv.Required(CONF_HID_ID): _hid_id,
            cv.Optional(CONF_GROUP): cv.declare_id(lv_group_t),
            cv.Optional(CONF_LONG_PRESS_TIME, default="400ms"): PRESS_TIME,
            cv.Optional(CONF_LONG_PRESS_REPEAT_TIME, default="100ms"): PRESS_TIME,
        },
        key=CONF_HID_ID,
    )
)

# Relative motion as a pointer with a cursor; the left button presses, the wheel scrolls.
HID_POINTERS_CONFIG = cv.ensure_list(
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.declare_id(LVHIDPointerListener),
            cv.Required(CONF_HID_ID): _hid_id,
            cv.Optional(CONF_CURSOR): lv_image,
            # The cursor hides when the pointer has not moved this long; the next motion shows it.
            cv.Optional(CONF_HIDE_AFTER): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LONG_PRESS_TIME, default="400ms"): PRESS_TIME,
            cv.Optional(CONF_LONG_PRESS_REPEAT_TIME, default="100ms"): PRESS_TIME,
        },
        key=CONF_HID_ID,
    )
)


def _keys_expression(keys) -> cg.RawExpression:
    items = ", ".join(
        f"{{{page}, {usage}, {mods}, {{{', '.join(str(o) for o in others)}}}}}"
        for page, usage, mods, others in keys
    )
    return cg.RawExpression(f"std::vector<{KeyBindingsKey}>{{{items}}}")


async def key_bindings_to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_preference_key(fnv1_hash(f"lvgl_key_bindings:{config[CONF_ID]}")))
    if CONF_FILE in config:
        cg.add_define("USE_LVGL_KEY_BINDINGS_FILE")
        cg.add(var.set_file(config[CONF_FILE]))
    configured = {action[CONF_NAME]: action for action in config[CONF_ACTIONS]}
    actions = [configured.get(name, {CONF_NAME: name}) for name in BUILTIN_ACTIONS]
    actions += [a for a in config[CONF_ACTIONS] if a[CONF_NAME] not in BUILTIN_ACTIONS]
    for action in actions:
        name = action[CONF_NAME]
        keys = action.get(CONF_KEYS)
        if keys is None:
            keys = [validate_key_combo(key) for key in BUILTIN_ACTIONS.get(name, [])]
        trigger = cg.nullptr
        if CONF_THEN in action:
            trigger = cg.new_Pvariable(action[CONF_TRIGGER_ID])
            await automation.build_automation(trigger, [], action)
        cg.add(var.add_action(name, _keys_expression(keys), trigger))
    if conf := config.get(CONF_ON_LEARNED):
        await automation.build_automation(
            var.get_learned_trigger(),
            [(cg.std_string, "action"), (cg.std_string, "key")],
            conf,
        )
    return var


async def hid_inputs_to_code(lv_component, config, default_group):
    bindings = None
    if config.get(CONF_HID_KEYBOARDS):
        add_lv_use("HID")
        bindings = await key_bindings_to_code(config[CONF_KEY_BINDINGS])
    for conf in config.get(CONF_HID_KEYBOARDS) or ():
        add_lv_use("HID")
        hid = await cg.get_variable(conf[CONF_HID_ID])
        listener = cg.new_Pvariable(
            conf[CONF_ID],
            hid,
            bindings,
            conf[CONF_LONG_PRESS_TIME].total_milliseconds,
            conf[CONF_LONG_PRESS_REPEAT_TIME].total_milliseconds,
        )
        await cg.register_parented(listener, lv_component)
        cg.add(lv_component.add_hid_keypad(listener))
        if group := conf.get(CONF_GROUP):
            group = lv_Pvariable(lv_group_t, group)
            lv_assign(group, lv_expr.group_create())
        else:
            group = default_group
        lv.indev_set_group(listener.get_drv(), group)
    for conf in config.get(CONF_HID_POINTERS) or ():
        add_lv_use("HID")
        hid = await cg.get_variable(conf[CONF_HID_ID])
        listener = cg.new_Pvariable(
            conf[CONF_ID],
            hid,
            conf[CONF_LONG_PRESS_TIME].total_milliseconds,
            conf[CONF_LONG_PRESS_REPEAT_TIME].total_milliseconds,
            lv_component,
        )
        if (cursor := conf.get(CONF_CURSOR)) is not None:
            add_lv_use("image")
            cg.add(listener.set_cursor_image(await lv_image.process(cursor)))
        else:
            cg.add(listener.set_default_cursor())
        if (hide_after := conf.get(CONF_HIDE_AFTER)) is not None:
            cg.add(listener.set_hide_after(hide_after.total_milliseconds))


KEY_BINDINGS_ID_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(KeyBindings)})


def _key_bindings_action(name, cls, schema):
    return automation.register_action(
        f"lvgl.key_bindings.{name}",
        lvgl_ns.class_(cls, automation.Action, cg.Parented.template(KeyBindings)),
        KEY_BINDINGS_ID_SCHEMA.extend(schema),
        synchronous=True,
    )


async def _key_bindings_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    for key in (CONF_ACTION, CONF_KEY):
        if key in config:
            value = await cg.templatable(config[key], args, cg.std_string)
            cg.add(getattr(var, f"set_{key}")(value))
    if CONF_TIMEOUT in config:
        timeout = config[CONF_TIMEOUT]
        if not cg.is_template(timeout):
            timeout = timeout.total_milliseconds
        cg.add(var.set_timeout(await cg.templatable(timeout, args, cg.uint32)))
    return var


_ACTION_NAME = cv.templatable(cv.string_strict)
_KEY_TEXT = cv.templatable(cv.string_strict)

for _name, _cls, _schema in (
    (
        "learn",
        "KeyBindingsLearnAction",
        {
            cv.Required(CONF_ACTION): _ACTION_NAME,
            cv.Optional(CONF_TIMEOUT, default="10s"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        },
    ),
    (
        "add",
        "KeyBindingsAddAction",
        {cv.Required(CONF_ACTION): _ACTION_NAME, cv.Required(CONF_KEY): _KEY_TEXT},
    ),
    ("remove", "KeyBindingsRemoveAction", {cv.Required(CONF_KEY): _KEY_TEXT}),
    ("clear", "KeyBindingsClearAction", {cv.Required(CONF_ACTION): _ACTION_NAME}),
    ("reset", "KeyBindingsResetAction", {}),
    ("cancel_learn", "KeyBindingsCancelLearnAction", {}),
    ("log", "KeyBindingsLogAction", {}),
):
    _key_bindings_action(_name, _cls, _schema)(_key_bindings_action_to_code)
