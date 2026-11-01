import esphome.codegen as cg
from esphome.components import event
import esphome.config_validation as cv

from .. import CONF_HID_ID, CONF_KEYS, HIDInput, hid_ns
from ..keys import KEY_NAMES, validate_key

HIDKeyEvent = hid_ns.class_("HIDKeyEvent", event.Event)


def _names(keys):
    by_code = {code: name for name, code in KEY_NAMES.items()}
    return [by_code.get(code, f"0x{code[0]:02X}:0x{code[1]:02X}") for code in keys]


# One event per key press, named after the key: remote buttons as Home Assistant events.
CONFIG_SCHEMA = event.event_schema(HIDKeyEvent).extend(
    {
        cv.GenerateID(CONF_HID_ID): cv.use_id(HIDInput),
        cv.Required(CONF_KEYS): cv.All(
            cv.ensure_list(validate_key), cv.Length(min=1, max=255)
        ),
    }
)


async def to_code(config):
    var = await event.new_event(config, event_types=_names(config[CONF_KEYS]))
    parent = await cg.get_variable(config[CONF_HID_ID])
    cg.add(var.attach(parent))
