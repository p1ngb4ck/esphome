import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_MESSAGE, CONF_STATE

from .. import CONF_SIP_PHONE_ID, SipPhone

CONF_PEER = "peer"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SIP_PHONE_ID): cv.use_id(SipPhone),
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(icon="mdi:phone"),
        cv.Optional(CONF_PEER): text_sensor.text_sensor_schema(icon="mdi:account"),
        cv.Optional(CONF_MESSAGE): text_sensor.text_sensor_schema(icon="mdi:message-text"),
    }
)


async def to_code(config):
    phone = await cg.get_variable(config[CONF_SIP_PHONE_ID])
    if state := config.get(CONF_STATE):
        sens = await text_sensor.new_text_sensor(state)
        cg.add(phone.set_state_text_sensor(sens))
    if peer := config.get(CONF_PEER):
        sens = await text_sensor.new_text_sensor(peer)
        cg.add(phone.set_peer_text_sensor(sens))
    if message := config.get(CONF_MESSAGE):
        sens = await text_sensor.new_text_sensor(message)
        cg.add(phone.set_message_text_sensor(sens))
