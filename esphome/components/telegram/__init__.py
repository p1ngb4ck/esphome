from esphome import automation
import esphome.codegen as cg
from esphome.components import audio, microphone, network
from esphome.components.http_request import CONF_HTTP_REQUEST_ID, HttpRequestComponent
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_ON_MESSAGE,
    CONF_PATH,
    CONF_TEXT,
    PLATFORM_ESP32,
)
from esphome.types import ConfigType

AUTO_LOAD = ["json", "audio", "ring_buffer"]
DEPENDENCIES = ["network", "http_request"]

CONF_TELEGRAM_ID = "telegram_id"
CONF_TOKEN = "token"
CONF_ALLOWED_CHATS = "allowed_chats"
CONF_DEFAULT_CHAT = "default_chat"
CONF_CHAT_ID = "chat_id"
CONF_POLL_TIMEOUT = "poll_timeout"
CONF_MAX_UPLOAD = "max_upload"
CONF_CAMERA = "camera"
CONF_MAX_VOICE = "max_voice"
CONF_CAPTION = "caption"
CONF_KIND = "kind"
CONF_FILE_ID = "file_id"
CONF_ON_FILE = "on_file"
CONF_ON_DOWNLOADED = "on_downloaded"
CONF_ON_SENT = "on_sent"
CONF_ON_HISTORY = "on_history"

CHAT_ID = cv.int_range(min=-(2**53), max=2**53)

telegram_ns = cg.esphome_ns.namespace("telegram")
TelegramBot = telegram_ns.class_("TelegramBot", cg.Component)
FileKind = telegram_ns.enum("FileKind", is_class=True)
FILE_KINDS = {
    "photo": FileKind.PHOTO,
    "voice": FileKind.VOICE,
    "audio": FileKind.AUDIO,
    "document": FileKind.DOCUMENT,
    "video": FileKind.VIDEO,
}

SendMessageAction = telegram_ns.class_("SendMessageAction", automation.Action)
SendFileAction = telegram_ns.class_("SendFileAction", automation.Action)
SendCameraPhotoAction = telegram_ns.class_("SendCameraPhotoAction", automation.Action)
DownloadFileAction = telegram_ns.class_("DownloadFileAction", automation.Action)
VoiceStartAction = telegram_ns.class_("VoiceStartAction", automation.Action)
VoiceSendAction = telegram_ns.class_("VoiceSendAction", automation.Action)
VoiceCancelAction = telegram_ns.class_("VoiceCancelAction", automation.Action)
IsRecordingCondition = telegram_ns.class_("IsRecordingCondition", automation.Condition)


def _request_opus(config: ConfigType) -> ConfigType:
    if CONF_MICROPHONE in config:
        audio.request_opus_support()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TelegramBot),
            cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
            cv.Optional(CONF_TOKEN): cv.string_strict,
            cv.Optional(CONF_ALLOWED_CHATS): cv.string,
            cv.Optional(CONF_DEFAULT_CHAT, default=0): CHAT_ID,
            cv.Optional(CONF_POLL_TIMEOUT, default="50s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=60)),
            ),
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
            cv.Optional(CONF_ON_FILE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_DOWNLOADED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_SENT): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_HISTORY): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    _request_opus,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    http = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    cg.add(var.set_http_request(http))
    if CONF_TOKEN in config:
        cg.add(var.set_token(config[CONF_TOKEN]))
    if CONF_ALLOWED_CHATS in config:
        cg.add(var.set_allowed_chats(config[CONF_ALLOWED_CHATS]))
    cg.add(var.set_default_chat(config[CONF_DEFAULT_CHAT]))
    cg.add(var.set_poll_timeout(config[CONF_POLL_TIMEOUT].total_seconds))
    cg.add(var.set_max_upload(config[CONF_MAX_UPLOAD]))
    if CONF_CAMERA in config:
        cg.add_define("USE_TELEGRAM_CAMERA")
        cam = await cg.get_variable(config[CONF_CAMERA])
        cg.add(var.set_camera(cam))
    if CONF_MICROPHONE in config:
        cg.add_define("USE_TELEGRAM_MICROPHONE")
        mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
        cg.add(var.set_microphone_source(mic_source))
        cg.add(var.set_max_voice_seconds(config[CONF_MAX_VOICE].total_seconds))
    await network.register_active_interface_listener(var)

    triggers = (
        (
            CONF_ON_MESSAGE,
            var.get_message_trigger(),
            [(cg.int64, "chat_id"), (cg.std_string, "sender"), (cg.std_string, "text")],
        ),
        (
            CONF_ON_FILE,
            var.get_file_trigger(),
            [
                (cg.int64, "chat_id"),
                (cg.std_string, "sender"),
                (cg.std_string, "kind"),
                (cg.std_string, "file_id"),
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
            [(cg.std_string, "kind"), (cg.std_string, "error")],
        ),
        (CONF_ON_HISTORY, var.get_history_trigger(), []),
    )
    for key, trigger, args in triggers:
        if key in config:
            await automation.build_automation(trigger, args, config[key])


TELEGRAM_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(TelegramBot)}
)


@automation.register_action(
    "telegram.send_message",
    SendMessageAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(TelegramBot),
            cv.Optional(CONF_CHAT_ID): cv.templatable(CHAT_ID),
            cv.Required(CONF_TEXT): cv.templatable(cv.string),
        },
        key=CONF_TEXT,
    ),
    synchronous=True,
)
async def send_message_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_CHAT_ID in config:
        cg.add(var.set_chat_id(await cg.templatable(config[CONF_CHAT_ID], args, cg.int64)))
    cg.add(var.set_text(await cg.templatable(config[CONF_TEXT], args, cg.std_string)))
    return var


@automation.register_action(
    "telegram.send_file",
    SendFileAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(TelegramBot),
            cv.Optional(CONF_CHAT_ID): cv.templatable(CHAT_ID),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
            cv.Optional(CONF_CAPTION): cv.templatable(cv.string),
            cv.Optional(CONF_KIND, default="document"): cv.enum(FILE_KINDS, lower=True),
        }
    ),
    synchronous=True,
)
async def send_file_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_CHAT_ID in config:
        cg.add(var.set_chat_id(await cg.templatable(config[CONF_CHAT_ID], args, cg.int64)))
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    if CONF_CAPTION in config:
        cg.add(var.set_caption(await cg.templatable(config[CONF_CAPTION], args, cg.std_string)))
    cg.add(var.set_kind(config[CONF_KIND]))
    return var


@automation.register_action(
    "telegram.send_camera_photo",
    SendCameraPhotoAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(TelegramBot),
            cv.Optional(CONF_CHAT_ID): cv.templatable(CHAT_ID),
            cv.Optional(CONF_CAPTION): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def send_camera_photo_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_CHAT_ID in config:
        cg.add(var.set_chat_id(await cg.templatable(config[CONF_CHAT_ID], args, cg.int64)))
    if CONF_CAPTION in config:
        cg.add(var.set_caption(await cg.templatable(config[CONF_CAPTION], args, cg.std_string)))
    return var


@automation.register_action(
    "telegram.download_file",
    DownloadFileAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(TelegramBot),
            cv.Required(CONF_FILE_ID): cv.templatable(cv.string),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def download_file_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_file_id(await cg.templatable(config[CONF_FILE_ID], args, cg.std_string)))
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


@automation.register_action(
    "telegram.voice_start", VoiceStartAction, TELEGRAM_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "telegram.voice_cancel", VoiceCancelAction, TELEGRAM_ACTION_SCHEMA, synchronous=True
)
async def simple_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "telegram.voice_send",
    VoiceSendAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(TelegramBot),
            cv.Optional(CONF_CHAT_ID): cv.templatable(CHAT_ID),
        }
    ),
    synchronous=True,
)
async def voice_send_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_CHAT_ID in config:
        cg.add(var.set_chat_id(await cg.templatable(config[CONF_CHAT_ID], args, cg.int64)))
    return var


@automation.register_condition(
    "telegram.is_recording", IsRecordingCondition, TELEGRAM_ACTION_SCHEMA
)
async def is_recording_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
