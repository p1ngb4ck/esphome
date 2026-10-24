import esphome.codegen as cg
from esphome.components import event
import esphome.config_validation as cv

from .. import CONF_SIP_PHONE_ID, SipPhone

EVENT_TYPES = [
    "registered",
    "unregistered",
    "registration_failed",
    "incoming",
    "outgoing",
    "answered",
    "ended",
    "missed",
    "rejected",
    "failed",
    "dtmf",
    "message",
    "message_sent",
    "message_failed",
]

CONFIG_SCHEMA = event.event_schema(event.Event, icon="mdi:phone-log").extend(
    {
        cv.GenerateID(CONF_SIP_PHONE_ID): cv.use_id(SipPhone),
    }
)


async def to_code(config):
    phone = await cg.get_variable(config[CONF_SIP_PHONE_ID])
    var = await event.new_event(config, event_types=EVENT_TYPES)
    cg.add(phone.set_call_event(var))
