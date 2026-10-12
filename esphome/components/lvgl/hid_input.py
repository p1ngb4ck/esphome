"""HID keyboards, remotes and mice (usb_hid, ble_client_hid, bt_hid) as LVGL input devices."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_GROUP, CONF_ID

from .defines import CONF_LONG_PRESS_REPEAT_TIME, CONF_LONG_PRESS_TIME, add_lv_use
from .lv_validation import lv_image
from .lvcode import lv, lv_assign, lv_expr, lv_Pvariable
from .schemas import PRESS_TIME
from .types import lv_group_t, lvgl_ns

CONF_HID_ID = "hid_id"
CONF_HID_KEYBOARDS = "hid_keyboards"
CONF_HID_POINTERS = "hid_pointers"
CONF_CURSOR = "cursor"

LVHIDKeypadListener = lvgl_ns.class_("LVHIDKeypadListener")
LVHIDPointerListener = lvgl_ns.class_("LVHIDPointerListener")


def _hid_input():
    # Imported late: hid is only loaded where a HID transport is configured.
    from esphome.components.hid import HIDInput

    return cv.use_id(HIDInput)


def _hid_id(value):
    return _hid_input()(value)


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
            cv.Optional(CONF_LONG_PRESS_TIME, default="400ms"): PRESS_TIME,
            cv.Optional(CONF_LONG_PRESS_REPEAT_TIME, default="100ms"): PRESS_TIME,
        },
        key=CONF_HID_ID,
    )
)


async def hid_inputs_to_code(lv_component, config, default_group):
    for conf in config.get(CONF_HID_KEYBOARDS) or ():
        add_lv_use("HID")
        hid = await cg.get_variable(conf[CONF_HID_ID])
        listener = cg.new_Pvariable(
            conf[CONF_ID],
            hid,
            conf[CONF_LONG_PRESS_TIME].total_milliseconds,
            conf[CONF_LONG_PRESS_REPEAT_TIME].total_milliseconds,
        )
        await cg.register_parented(listener, lv_component)
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
