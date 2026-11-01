from esphome import automation
import esphome.codegen as cg
from esphome.components import audio, esp32, network, psram, speaker
from esphome.components.resampler.microphone import ResamplerMicrophone
import esphome.config_validation as cv
from esphome.const import (
    CONF_DOMAIN,
    CONF_ID,
    CONF_MICROPHONE,
    CONF_NAME,
    CONF_ON_MESSAGE,
    CONF_ON_STATE,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_SPEAKER,
    CONF_TASK_STACK_IN_PSRAM,
    CONF_TEXT,
    CONF_USERNAME,
    CONF_VALUE,
    PLATFORM_ESP32,
)
from esphome.core import CORE
from esphome.types import ConfigType

AUTO_LOAD = ["audio", "md5", "ring_buffer"]
DEPENDENCIES = ["network"]

CONF_SIP_PHONE_ID = "sip_phone_id"
CONF_SIP_PORT = "sip_port"
CONF_RTP_PORT = "rtp_port"
CONF_ACCOUNT = "account"
CONF_SERVER = "server"
CONF_AUTH_USERNAME = "auth_username"
CONF_DISPLAY_NAME = "display_name"
CONF_REGISTER_EXPIRY = "register_expiry"
CONF_CODECS = "codecs"
CONF_OPUS = "opus"
CONF_BITRATE = "bitrate"
CONF_COMPLEXITY = "complexity"
CONF_PTIME = "ptime"
CONF_PLAYOUT_DELAY = "playout_delay"
CONF_RING_TIMEOUT = "ring_timeout"
CONF_CONTACTS = "contacts"
CONF_URI = "uri"
CONF_DESTINATION = "destination"
CONF_DIGITS = "digits"
CONF_ON_REGISTERED = "on_registered"
CONF_ON_REGISTRATION_FAILED = "on_registration_failed"
CONF_ON_INCOMING_CALL = "on_incoming_call"
CONF_ON_OUTGOING_CALL = "on_outgoing_call"
CONF_ON_CALL_ESTABLISHED = "on_call_established"
CONF_ON_CALL_ENDED = "on_call_ended"
CONF_ON_DTMF = "on_dtmf"

sip_phone_ns = cg.esphome_ns.namespace("sip_phone")
SipPhone = sip_phone_ns.class_("SipPhone", cg.Component)
Codec = sip_phone_ns.enum("Codec", is_class=True)
CODECS = {
    "opus": Codec.OPUS,
    "g722": Codec.G722,
    "pcma": Codec.PCMA,
    "pcmu": Codec.PCMU,
}

CallAction = sip_phone_ns.class_("CallAction", automation.Action)
AnswerAction = sip_phone_ns.class_("AnswerAction", automation.Action)
HangupAction = sip_phone_ns.class_("HangupAction", automation.Action)
DeclineAction = sip_phone_ns.class_("DeclineAction", automation.Action)
SendDtmfAction = sip_phone_ns.class_("SendDtmfAction", automation.Action)
SendMessageAction = sip_phone_ns.class_("SendMessageAction", automation.Action)
SetMuteAction = sip_phone_ns.class_("SetMuteAction", automation.Action)
SetDndAction = sip_phone_ns.class_("SetDndAction", automation.Action)
SetAutoAnswerAction = sip_phone_ns.class_("SetAutoAnswerAction", automation.Action)

ACCOUNT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SERVER): cv.string_strict,
        cv.Optional(CONF_PORT, default=5060): cv.port,
        cv.Required(CONF_USERNAME): cv.All(cv.string_strict, cv.Length(max=63)),
        cv.Required(CONF_PASSWORD): cv.All(cv.string, cv.Length(max=63)),
        cv.Optional(CONF_AUTH_USERNAME, default=""): cv.All(cv.string, cv.Length(max=63)),
        cv.Optional(CONF_DOMAIN, default=""): cv.All(cv.string, cv.Length(max=63)),
        cv.Optional(CONF_DISPLAY_NAME, default=""): cv.All(cv.string, cv.Length(max=47)),
        cv.Optional(CONF_REGISTER_EXPIRY, default="600s"): cv.All(
            cv.positive_time_period_seconds, cv.Range(min=cv.TimePeriod(seconds=60))
        ),
    }
)

CONTACT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(max=31)),
        cv.Required(CONF_URI): cv.All(cv.string_strict, cv.Length(max=95)),
    }
)

OPUS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_BITRATE, default=64000): cv.int_range(6000, 510000),
        cv.Optional(CONF_COMPLEXITY, default=2): cv.int_range(0, 10),
    }
)


def _unique_codecs(value):
    if len(set(value)) != len(value):
        raise cv.Invalid("each codec only once")
    return value


def _validate_ptime(value):
    if value.total_milliseconds not in (20, 40, 60):
        raise cv.Invalid("ptime must be 20ms, 40ms or 60ms")
    return value


def _request_codecs(config: ConfigType) -> ConfigType:
    if "opus" in config[CONF_CODECS]:
        audio.request_opus_support()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SipPhone),
            cv.Required(CONF_MICROPHONE): cv.use_id(ResamplerMicrophone),
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_SIP_PORT, default=5060): cv.port,
            cv.Optional(CONF_RTP_PORT, default=40000): cv.port,
            cv.Optional(CONF_ACCOUNT): ACCOUNT_SCHEMA,
            cv.Optional(CONF_CODECS, default=["opus", "g722", "pcma", "pcmu"]): cv.All(
                cv.ensure_list(cv.one_of(*CODECS, lower=True)),
                cv.Length(min=1, max=4),
                _unique_codecs,
            ),
            cv.Optional(CONF_OPUS, default={}): OPUS_SCHEMA,
            cv.Optional(CONF_PTIME, default="20ms"): cv.All(
                cv.positive_time_period_milliseconds, _validate_ptime
            ),
            cv.Optional(CONF_PLAYOUT_DELAY, default="60ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(max=cv.TimePeriod(milliseconds=500)),
            ),
            cv.Optional(CONF_RING_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TASK_STACK_IN_PSRAM, default=False): psram.validate_task_stack_in_psram,
            cv.Optional(CONF_CONTACTS, default=[]): cv.All(
                cv.ensure_list(CONTACT_SCHEMA), cv.Length(max=24)
            ),
            cv.Optional(CONF_ON_REGISTERED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_REGISTRATION_FAILED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_INCOMING_CALL): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_OUTGOING_CALL): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_CALL_ESTABLISHED): automation.validate_automation(
                single=True
            ),
            cv.Optional(CONF_ON_CALL_ENDED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_DTMF): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_STATE): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    _request_codecs,
)


async def to_code(config: ConfigType) -> None:
    create = cg.MockObj(str(SipPhone), "::").create()
    var = cg.Pvariable(config[CONF_ID], create)
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))
    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))
    cg.add(var.set_sip_port(config[CONF_SIP_PORT]))
    cg.add(var.set_rtp_port(config[CONF_RTP_PORT]))
    cg.add(var.set_identity(CORE.name))
    for codec in config[CONF_CODECS]:
        cg.add(var.add_codec(CODECS[codec]))
    if "opus" in config[CONF_CODECS]:
        cg.add_define("USE_SIP_PHONE_OPUS")
    opus = config[CONF_OPUS]
    cg.add(var.set_opus(opus[CONF_BITRATE], opus[CONF_COMPLEXITY]))
    cg.add(var.set_ptime(config[CONF_PTIME].total_milliseconds))
    cg.add(var.set_playout_delay(config[CONF_PLAYOUT_DELAY].total_milliseconds))
    cg.add(var.set_ring_timeout(config[CONF_RING_TIMEOUT].total_milliseconds))
    cg.add(var.set_task_stack_in_psram(config[CONF_TASK_STACK_IN_PSRAM]))
    if account := config.get(CONF_ACCOUNT):
        cg.add(
            var.set_default_account(
                account[CONF_SERVER],
                account[CONF_PORT],
                account[CONF_USERNAME],
                account[CONF_AUTH_USERNAME],
                account[CONF_PASSWORD],
                account[CONF_DOMAIN],
                account[CONF_DISPLAY_NAME],
                account[CONF_REGISTER_EXPIRY].total_seconds,
            )
        )
    for contact in config[CONF_CONTACTS]:
        cg.add(var.add_config_contact(contact[CONF_NAME], contact[CONF_URI]))

    esp32.require_vfs_select()
    await network.register_active_interface_listener(var)

    triggers = (
        (CONF_ON_REGISTERED, var.get_registered_trigger(), []),
        (CONF_ON_REGISTRATION_FAILED, var.get_registration_failed_trigger(), [(cg.std_string, "reason")]),
        (
            CONF_ON_INCOMING_CALL,
            var.get_incoming_call_trigger(),
            [(cg.std_string, "uri"), (cg.std_string, "name")],
        ),
        (CONF_ON_OUTGOING_CALL, var.get_outgoing_call_trigger(), [(cg.std_string, "uri")]),
        (CONF_ON_CALL_ESTABLISHED, var.get_call_established_trigger(), []),
        (CONF_ON_CALL_ENDED, var.get_call_ended_trigger(), [(cg.std_string, "reason")]),
        (CONF_ON_DTMF, var.get_dtmf_trigger(), [(cg.std_string, "digit")]),
        (
            CONF_ON_MESSAGE,
            var.get_message_trigger(),
            [(cg.std_string, "uri"), (cg.std_string, "text")],
        ),
        (CONF_ON_STATE, var.get_state_trigger(), [(cg.std_string, "state")]),
    )
    for key, trigger, args in triggers:
        if key in config:
            await automation.build_automation(trigger, args, config[key])


SIP_PHONE_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(SipPhone)}
)


@automation.register_action(
    "sip_phone.call",
    CallAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(SipPhone),
            cv.Required(CONF_DESTINATION): cv.templatable(cv.string),
        },
        key=CONF_DESTINATION,
    ),
    synchronous=True,
)
async def call_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_destination(await cg.templatable(config[CONF_DESTINATION], args, cg.std_string)))
    return var


@automation.register_action(
    "sip_phone.answer", AnswerAction, SIP_PHONE_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "sip_phone.hangup", HangupAction, SIP_PHONE_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "sip_phone.decline", DeclineAction, SIP_PHONE_ACTION_SCHEMA, synchronous=True
)
async def simple_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "sip_phone.send_dtmf",
    SendDtmfAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(SipPhone),
            cv.Required(CONF_DIGITS): cv.templatable(cv.string),
        },
        key=CONF_DIGITS,
    ),
    synchronous=True,
)
async def send_dtmf_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_digits(await cg.templatable(config[CONF_DIGITS], args, cg.std_string)))
    return var


@automation.register_action(
    "sip_phone.send_message",
    SendMessageAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SipPhone),
            cv.Required(CONF_DESTINATION): cv.templatable(cv.string),
            cv.Required(CONF_TEXT): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def send_message_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_destination(await cg.templatable(config[CONF_DESTINATION], args, cg.std_string)))
    cg.add(var.set_text(await cg.templatable(config[CONF_TEXT], args, cg.std_string)))
    return var


SET_FLAG_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(SipPhone),
        cv.Required(CONF_VALUE): cv.templatable(cv.boolean),
    },
    key=CONF_VALUE,
)


@automation.register_action(
    "sip_phone.set_mute", SetMuteAction, SET_FLAG_SCHEMA, synchronous=True
)
@automation.register_action(
    "sip_phone.set_dnd", SetDndAction, SET_FLAG_SCHEMA, synchronous=True
)
@automation.register_action(
    "sip_phone.set_auto_answer", SetAutoAnswerAction, SET_FLAG_SCHEMA, synchronous=True
)
async def set_flag_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_value(await cg.templatable(config[CONF_VALUE], args, bool)))
    return var
