from esphome import automation
import esphome.codegen as cg
from esphome.components import audio, microphone, network, speaker
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.components.resampler.microphone import ResamplerMicrophone
from esphome.components.mtproto import CONF_MTPROTO_ID, Mtproto
from esphome.components.http_request import CONF_HTTP_REQUEST_ID, HttpRequestComponent
import esphome.config_validation as cv
from esphome.const import (
    CONF_CHANNEL,
    CONF_ID,
    CONF_MICROPHONE,
    CONF_ON_MESSAGE,
    CONF_PATH,
    CONF_PAYLOAD,
    CONF_SIZE,
    CONF_SPEAKER,
    CONF_TEXT,
    PLATFORM_ESP32,
)
from esphome.types import ConfigType

AUTO_LOAD = ["json", "audio", "ring_buffer", "mtproto"]
DEPENDENCIES = ["network", "http_request"]

CONF_TELEGRAM = "telegram"
CONF_WHATSAPP = "whatsapp"
CONF_FACEBOOK = "facebook"
CONF_TRANSPORT = "transport"
CONF_ALLOWED_CHATS = "allowed_chats"
CONF_DEFAULT_CHAT = "default_chat"
CONF_DEFAULT_CHANNEL = "default_channel"
CONF_PHONE_NUMBER_ID = "phone_number_id"
CONF_API_VERSION = "api_version"
CONF_SPOOL_DIR = "spool_dir"
CONF_TOKEN = "token"
CONF_CHAT_ID = "chat_id"
CONF_POLL_TIMEOUT = "poll_timeout"
CONF_MAX_UPLOAD = "max_upload"
CONF_CAMERA = "camera"
CONF_MAX_VOICE = "max_voice"
CONF_CAPTION = "caption"
CONF_KIND = "kind"
CONF_FILE_REF = "file_ref"
CONF_FILE_NAME = "file_name"
CONF_SENDER = "sender"
CONF_ON_FILE = "on_file"
CONF_ON_DOWNLOADED = "on_downloaded"
CONF_ON_SENT = "on_sent"
CONF_ON_HISTORY = "on_history"
CONF_ON_OUTBOUND = "on_outbound"
CONF_ON_ACCOUNT = "on_account"
CONF_ON_PHOTO = "on_photo"
CONF_ON_CALL = "on_call"
CONF_CALL = "call"

CHAT_ID = cv.int_range(min=-(2**53), max=2**53)

messenger_ns = cg.esphome_ns.namespace("messenger")
Messenger = messenger_ns.class_("Messenger", cg.Component)
Channel = messenger_ns.enum("Channel", is_class=True)
CHANNELS = {
    CONF_TELEGRAM: Channel.TELEGRAM,
    CONF_WHATSAPP: Channel.WHATSAPP,
    CONF_FACEBOOK: Channel.FACEBOOK,
}
Transport = messenger_ns.enum("Transport", is_class=True)
TRANSPORTS = {
    "direct": Transport.DIRECT,
    "home_assistant": Transport.HOME_ASSISTANT,
    "account": Transport.ACCOUNT,
}
FileKind = messenger_ns.enum("FileKind", is_class=True)
FILE_KINDS = {
    "photo": FileKind.PHOTO,
    "voice": FileKind.VOICE,
    "audio": FileKind.AUDIO,
    "document": FileKind.DOCUMENT,
    "video": FileKind.VIDEO,
}

SendMessageAction = messenger_ns.class_("SendMessageAction", automation.Action)
SendFileAction = messenger_ns.class_("SendFileAction", automation.Action)
SendCameraPhotoAction = messenger_ns.class_("SendCameraPhotoAction", automation.Action)
DownloadFileAction = messenger_ns.class_("DownloadFileAction", automation.Action)
VoiceStartAction = messenger_ns.class_("VoiceStartAction", automation.Action)
VoiceSendAction = messenger_ns.class_("VoiceSendAction", automation.Action)
VoiceCancelAction = messenger_ns.class_("VoiceCancelAction", automation.Action)
ReceiveAction = messenger_ns.class_("ReceiveAction", automation.Action)
ReceiveWebhookAction = messenger_ns.class_("ReceiveWebhookAction", automation.Action)
IsRecordingCondition = messenger_ns.class_("IsRecordingCondition", automation.Condition)

CHANNEL_BASE = {
    cv.Optional(CONF_TRANSPORT, default="direct"): cv.enum(TRANSPORTS, lower=True),
    cv.Optional(CONF_TOKEN): cv.string_strict,
    cv.Optional(CONF_ALLOWED_CHATS): cv.string,
    cv.Optional(CONF_DEFAULT_CHAT, default=0): CHAT_ID,
}

CALL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MICROPHONE): cv.use_id(ResamplerMicrophone),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
    }
)

TELEGRAM_SCHEMA = cv.Schema(
    {
        **CHANNEL_BASE,
        cv.Optional(CONF_CALL): CALL_SCHEMA,
        cv.Optional(CONF_POLL_TIMEOUT, default="50s"): cv.All(
            cv.positive_time_period_seconds,
            cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=60)),
        ),
    }
)
WHATSAPP_SCHEMA = cv.Schema(
    {
        **CHANNEL_BASE,
        cv.Optional(CONF_PHONE_NUMBER_ID): cv.string_strict,
    }
)
FACEBOOK_SCHEMA = cv.Schema(CHANNEL_BASE)


def _request_opus(config: ConfigType) -> ConfigType:
    if CONF_MICROPHONE in config or CONF_CALL in config.get(CONF_TELEGRAM, {}):
        audio.request_opus_support()
    if CONF_CALL in config.get(CONF_TELEGRAM, {}):
        network.require_ipv6("messenger")(config)
    return config


def _validate_channels(config: ConfigType) -> ConfigType:
    enabled = [name for name in CHANNELS if name in config]
    if not enabled:
        raise cv.Invalid("at least one of telegram, whatsapp, facebook is required")
    if CONF_DEFAULT_CHANNEL in config and config[CONF_DEFAULT_CHANNEL] not in enabled:
        raise cv.Invalid(f"default_channel {config[CONF_DEFAULT_CHANNEL]} is not configured")
    if CONF_TELEGRAM in config and CONF_CALL in config[CONF_TELEGRAM]:
        if config[CONF_TELEGRAM][CONF_TRANSPORT] != "account":
            raise cv.Invalid("telegram call needs transport account (the panel's own Telegram account)")
    for name in (CONF_WHATSAPP, CONF_FACEBOOK):
        if name in config and config[name][CONF_TRANSPORT] == "account":
            raise cv.Invalid(f"transport account is only for telegram (its own user account), not {name}")
    via_ha = any(config[name][CONF_TRANSPORT] == "home_assistant" for name in enabled)
    spools = CONF_CAMERA in config or CONF_MICROPHONE in config
    if via_ha and spools and CONF_SPOOL_DIR not in config:
        raise cv.Invalid(
            "spool_dir is required: camera photos and voice notes go to Home Assistant as files"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Messenger),
            cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
            cv.GenerateID(CONF_MTPROTO_ID): cv.use_id(Mtproto),
            cv.Optional(CONF_TELEGRAM): TELEGRAM_SCHEMA,
            cv.Optional(CONF_WHATSAPP): WHATSAPP_SCHEMA,
            cv.Optional(CONF_FACEBOOK): FACEBOOK_SCHEMA,
            cv.Optional(CONF_DEFAULT_CHANNEL): cv.one_of(*CHANNELS, lower=True),
            cv.Optional(CONF_API_VERSION, default="v26.0"): cv.All(
                cv.string_strict, cv.Length(max=7)
            ),
            cv.Optional(CONF_SPOOL_DIR): cv.All(cv.string_strict, cv.Length(max=160)),
            cv.Optional(CONF_MAX_UPLOAD, default="4MB"): cv.All(
                cv.validate_bytes, cv.int_range(min=1024, max=50 * 1024 * 1024)
            ),
            cv.Optional(CONF_CAMERA): cv.use_id(cg.Component),
            cv.Optional(CONF_MICROPHONE): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=16,
                min_channels=1,
                max_channels=1,
            ),
            cv.Optional(CONF_MAX_VOICE, default="60s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=300)),
            ),
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_ACCOUNT): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_FILE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_DOWNLOADED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_SENT): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_HISTORY): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_PHOTO): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_CALL): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_OUTBOUND): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    _validate_channels,
    _request_opus,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    http = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    cg.add(var.set_http_request(http))
    cg.add(var.set_api_version(config[CONF_API_VERSION]))
    if CONF_SPOOL_DIR in config:
        cg.add(var.set_spool_dir(config[CONF_SPOOL_DIR].rstrip("/")))
    enabled = [name for name in CHANNELS if name in config]
    cg.add(var.set_default_channel(CHANNELS[config.get(CONF_DEFAULT_CHANNEL, enabled[0])]))
    for name in enabled:
        conf = config[name]
        channel = CHANNELS[name]
        cg.add(var.enable_channel(channel, TRANSPORTS[conf[CONF_TRANSPORT]]))
        if CONF_TOKEN in conf:
            cg.add(var.set_token(channel, conf[CONF_TOKEN]))
        if CONF_ALLOWED_CHATS in conf:
            cg.add(var.set_allowed_chats(channel, conf[CONF_ALLOWED_CHATS]))
        if conf[CONF_DEFAULT_CHAT] != 0:
            cg.add(var.set_default_chat(channel, conf[CONF_DEFAULT_CHAT]))
        if CONF_PHONE_NUMBER_ID in conf:
            cg.add(var.set_account(channel, conf[CONF_PHONE_NUMBER_ID]))
        if CONF_POLL_TIMEOUT in conf:
            cg.add(var.set_poll_timeout(conf[CONF_POLL_TIMEOUT].total_seconds))
    cg.add(var.set_max_upload(config[CONF_MAX_UPLOAD]))
    if CONF_CAMERA in config:
        cg.add_define("USE_MESSENGER_CAMERA")
        cam = await cg.get_variable(config[CONF_CAMERA])
        cg.add(var.set_camera(cam))
    if CONF_MICROPHONE in config:
        cg.add_define("USE_MESSENGER_MICROPHONE")
        mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
        cg.add(var.set_microphone_source(mic_source))
        cg.add(var.set_max_voice_seconds(config[CONF_MAX_VOICE].total_seconds))
    if CONF_TELEGRAM in config and config[CONF_TELEGRAM][CONF_TRANSPORT] == "direct":
        await network.register_active_interface_listener(var)
    if CONF_TELEGRAM in config and config[CONF_TELEGRAM][CONF_TRANSPORT] == "account":
        cg.add_define("USE_MESSENGER_ACCOUNT")
        mtproto = await cg.get_variable(config[CONF_MTPROTO_ID])
        cg.add(mtproto.enable())
        cg.add(var.set_mtproto(mtproto))
        if CONF_ON_ACCOUNT in config:
            await automation.build_automation(
                var.get_account_trigger(), [(cg.uint8, "changes")], config[CONF_ON_ACCOUNT]
            )
        if call := config[CONF_TELEGRAM].get(CONF_CALL):
            cg.add_define("USE_MESSENGER_CALL")
            call_mic = await cg.get_variable(call[CONF_MICROPHONE])
            call_spk = await cg.get_variable(call[CONF_SPEAKER])
            cg.add(var.set_call_audio(call_mic, call_spk))
            add_idf_component(name="espressif/esp_peer", ref="1.5.7")
            add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_PROTO_DTLS", True)
            add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_DTLS_SRTP", True)
            add_idf_sdkconfig_option("CONFIG_MBEDTLS_X509_CREATE_C", True)
            if CONF_ON_CALL in config:
                await automation.build_automation(
                    var.get_call_trigger(), [(cg.uint8, "state")], config[CONF_ON_CALL]
                )

    triggers = (
        (
            CONF_ON_MESSAGE,
            var.get_message_trigger(),
            [
                (cg.std_string, "channel"),
                (cg.int64, "chat_id"),
                (cg.std_string, "sender"),
                (cg.std_string, "text"),
            ],
        ),
        (
            CONF_ON_FILE,
            var.get_file_trigger(),
            [
                (cg.std_string, "channel"),
                (cg.int64, "chat_id"),
                (cg.std_string, "sender"),
                (cg.std_string, "kind"),
                (cg.std_string, "file_ref"),
                (cg.std_string, "file_name"),
                (cg.uint32, "size"),
            ],
        ),
        (
            CONF_ON_DOWNLOADED,
            var.get_downloaded_trigger(),
            [(cg.std_string, "path"), (cg.std_string, "error")],
        ),
        (
            CONF_ON_SENT,
            var.get_sent_trigger(),
            [
                (cg.std_string, "channel"),
                (cg.std_string, "kind"),
                (cg.std_string, "error"),
            ],
        ),
        (CONF_ON_HISTORY, var.get_history_trigger(), []),
        (CONF_ON_PHOTO, var.get_photo_trigger(), []),
        (
            CONF_ON_OUTBOUND,
            var.get_outbound_trigger(),
            [
                (cg.std_string, "channel"),
                (cg.int64, "chat_id"),
                (cg.std_string, "kind"),
                (cg.std_string, "text"),
                (cg.std_string, "path"),
            ],
        ),
    )
    for key, trigger, args in triggers:
        if key in config:
            await automation.build_automation(trigger, args, config[key])


TARGET = {
    cv.GenerateID(): cv.use_id(Messenger),
    cv.Optional(CONF_CHANNEL): cv.templatable(cv.string),
    cv.Optional(CONF_CHAT_ID): cv.templatable(CHAT_ID),
}


async def _target_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_CHANNEL in config:
        cg.add(var.set_channel(await cg.templatable(config[CONF_CHANNEL], args, cg.std_string)))
    if CONF_CHAT_ID in config:
        cg.add(var.set_chat_id(await cg.templatable(config[CONF_CHAT_ID], args, cg.int64)))
    return var


@automation.register_action(
    "messenger.send_message",
    SendMessageAction,
    cv.maybe_simple_value(
        {**TARGET, cv.Required(CONF_TEXT): cv.templatable(cv.string)},
        key=CONF_TEXT,
    ),
    synchronous=True,
)
async def send_message_action_to_code(config, action_id, template_arg, args):
    var = await _target_to_code(config, action_id, template_arg, args)
    cg.add(var.set_text(await cg.templatable(config[CONF_TEXT], args, cg.std_string)))
    return var


@automation.register_action(
    "messenger.send_file",
    SendFileAction,
    cv.Schema(
        {
            **TARGET,
            cv.Required(CONF_PATH): cv.templatable(cv.string),
            cv.Optional(CONF_CAPTION): cv.templatable(cv.string),
            cv.Optional(CONF_KIND, default="document"): cv.enum(FILE_KINDS, lower=True),
        }
    ),
    synchronous=True,
)
async def send_file_action_to_code(config, action_id, template_arg, args):
    var = await _target_to_code(config, action_id, template_arg, args)
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    if CONF_CAPTION in config:
        cg.add(var.set_caption(await cg.templatable(config[CONF_CAPTION], args, cg.std_string)))
    cg.add(var.set_kind(config[CONF_KIND]))
    return var


@automation.register_action(
    "messenger.send_camera_photo",
    SendCameraPhotoAction,
    cv.Schema({**TARGET, cv.Optional(CONF_CAPTION): cv.templatable(cv.string)}),
    synchronous=True,
)
async def send_camera_photo_action_to_code(config, action_id, template_arg, args):
    var = await _target_to_code(config, action_id, template_arg, args)
    if CONF_CAPTION in config:
        cg.add(var.set_caption(await cg.templatable(config[CONF_CAPTION], args, cg.std_string)))
    return var


@automation.register_action(
    "messenger.download_file",
    DownloadFileAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(Messenger),
            cv.Optional(CONF_CHANNEL): cv.templatable(cv.string),
            cv.Required(CONF_FILE_REF): cv.templatable(cv.string),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def download_file_action_to_code(config, action_id, template_arg, args):
    var = await _target_to_code(config, action_id, template_arg, args)
    cg.add(var.set_file_ref(await cg.templatable(config[CONF_FILE_REF], args, cg.std_string)))
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


MESSENGER_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(Messenger)})


@automation.register_action(
    "messenger.voice_start", VoiceStartAction, MESSENGER_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "messenger.voice_cancel", VoiceCancelAction, MESSENGER_ACTION_SCHEMA, synchronous=True
)
async def simple_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "messenger.voice_send", VoiceSendAction, cv.Schema(TARGET), synchronous=True
)
async def voice_send_action_to_code(config, action_id, template_arg, args):
    return await _target_to_code(config, action_id, template_arg, args)


@automation.register_action(
    "messenger.receive",
    ReceiveAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(Messenger),
            cv.Required(CONF_CHANNEL): cv.templatable(cv.string),
            cv.Required(CONF_CHAT_ID): cv.templatable(CHAT_ID),
            cv.Optional(CONF_SENDER): cv.templatable(cv.string),
            cv.Optional(CONF_TEXT): cv.templatable(cv.string),
            cv.Optional(CONF_KIND): cv.templatable(cv.string),
            cv.Optional(CONF_FILE_REF): cv.templatable(cv.string),
            cv.Optional(CONF_FILE_NAME): cv.templatable(cv.string),
            cv.Optional(CONF_SIZE): cv.templatable(cv.uint32_t),
        }
    ),
    synchronous=True,
)
async def receive_action_to_code(config, action_id, template_arg, args):
    var = await _target_to_code(config, action_id, template_arg, args)
    for key, setter in (
        (CONF_SENDER, var.set_sender),
        (CONF_TEXT, var.set_text),
        (CONF_KIND, var.set_kind),
        (CONF_FILE_REF, var.set_file_ref),
        (CONF_FILE_NAME, var.set_file_name),
    ):
        if key in config:
            cg.add(setter(await cg.templatable(config[key], args, cg.std_string)))
    if CONF_SIZE in config:
        cg.add(var.set_size(await cg.templatable(config[CONF_SIZE], args, cg.uint32)))
    return var


@automation.register_action(
    "messenger.receive_webhook",
    ReceiveWebhookAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(Messenger),
            cv.Required(CONF_PAYLOAD): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def receive_webhook_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_payload(await cg.templatable(config[CONF_PAYLOAD], args, cg.std_string)))
    return var


@automation.register_condition(
    "messenger.is_recording", IsRecordingCondition, MESSENGER_ACTION_SCHEMA
)
async def is_recording_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
