from collections.abc import Callable

from esphome import automation
import esphome.codegen as cg
from esphome.components.const import CONF_BITRATE
import esphome.config_validation as cv
from esphome.const import (
    CONF_ENTITY_CATEGORY,
    CONF_FORMAT,
    CONF_ICON,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_ON_IDLE,
    CONF_ON_STATE,
    CONF_ON_TURN_OFF,
    CONF_ON_TURN_ON,
    CONF_SAMPLE_RATE,
    CONF_STATUS,
    CONF_VOLUME,
)
from esphome.core import CORE
from esphome.core.entity_helpers import (
    entity_duplicate_validator,
    inherit_property_from,
    queue_entity_register,
    setup_entity,
)
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.cpp_generator import MockObj, MockObjClass
from esphome.types import ConfigType

CODEOWNERS = ["@jesserockz"]


def AUTO_LOAD(config=None) -> list[str]:
    for entry in config if isinstance(config, list) else []:
        if not isinstance(entry, dict):
            continue
        extended = entry.get(CONF_EXTENDED_API_SERVICES)
        queue = entry.get(CONF_QUEUE)
        if (isinstance(extended, dict) and extended.get(CONF_COVER_CANVASES)) or (
            isinstance(queue, dict) and queue.get(CONF_RELAY)
        ):
            return ["relay_link"]
    return []


DOMAIN = "media_player"

IS_PLATFORM_COMPONENT = True

media_player_ns = cg.esphome_ns.namespace("media_player")

MediaPlayer = media_player_ns.class_("MediaPlayer")
ExtendedApiServices = media_player_ns.class_("ExtendedApiServices", cg.Component)
MediaPictures = media_player_ns.class_("MediaPictures", cg.Component)
PicturesShowAction = media_player_ns.class_("PicturesShowAction", automation.Action)
PicturesOpenAction = media_player_ns.class_("PicturesOpenAction", automation.Action)
PicturesIntervalAction = media_player_ns.class_("PicturesIntervalAction", automation.Action)
PicturesThumbnailAction = media_player_ns.class_("PicturesThumbnailAction", automation.Action)
PicturesCommandAction = media_player_ns.class_("PicturesCommandAction", automation.Action)

CONF_PICTURES = "pictures"
CONF_CONTROLS = "controls"
CONF_QUEUE = "queue"
CONF_MAX_ENTRIES = "max_entries"
CONF_RELAY = "relay"
MediaQueue = media_player_ns.class_("MediaQueue", cg.Component)
QueueAddAction = media_player_ns.class_("QueueAddAction", automation.Action)
QueuePlayAction = media_player_ns.class_("QueuePlayAction", automation.Action)
QueueModeAction = media_player_ns.class_("QueueModeAction", automation.Action)
QueueCommandAction = media_player_ns.class_("QueueCommandAction", automation.Action)
QUEUE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(MediaQueue),
        cv.Optional(CONF_MAX_ENTRIES, default=500): cv.int_range(min=1, max=5000),
        cv.Optional(CONF_RELAY, default=False): cv.boolean,
    }
)


async def _queue_to_code(player, conf):
    cg.add_define("USE_MEDIA_PLAYER_QUEUE")
    var = cg.Pvariable(conf[CONF_ID], cg.MockObj(str(MediaQueue), "::").create())
    await cg.register_component(var, conf)
    cg.add(var.set_player(player))
    cg.add(var.set_max_entries(conf[CONF_MAX_ENTRIES]))
    if conf[CONF_RELAY]:
        from esphome.components import relay_link

        cg.add_define("USE_MEDIA_PLAYER_QUEUE_RELAY")
        cg.add_define("USE_RELAY_LINK_CHANNELS")
        cg.add(var.set_relay(await relay_link.get_relay_link()))
    return var
CONF_HIDE_AFTER = "hide_after"
CONF_BUTTONS = "buttons"
CONF_TITLE = "title"
CONF_BAR_STYLE_ID = "bar_style_id"
CONF_BUTTON_STYLE_ID = "button_style_id"
CONF_ACCENT_BUTTON_STYLE_ID = "accent_button_style_id"
CONF_FOCUS_STYLE_ID = "focus_style_id"
CONF_ON_EXIT = "on_exit"
MediaControls = media_player_ns.class_("MediaControls", cg.Component)
ControlsAction = media_player_ns.class_("ControlsAction", automation.Action)
ControlsTitleAction = media_player_ns.class_("ControlsTitleAction", automation.Action)
CONTROL_BUTTONS = {
    "previous": 0,
    "play_pause": 1,
    "stop": 2,
    "next": 3,
    "volume_down": 4,
    "volume_up": 5,
    "exit": 6,
}


def _controls(value):
    from esphome.components.lvgl.lvcode import LvglComponent
    from esphome.components.lvgl.types import lv_style_t

    return cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MediaControls),
            cv.GenerateID(CONF_LVGL_ID): cv.use_id(LvglComponent),
            cv.Optional(CONF_HIDE_AFTER, default="5s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BUTTONS, default=["previous", "play_pause", "stop", "next"]): cv.ensure_list(
                cv.one_of(*CONTROL_BUTTONS, lower=True)
            ),
            cv.Optional(CONF_TITLE, default=True): cv.boolean,
            cv.Optional(CONF_BAR_STYLE_ID): cv.use_id(lv_style_t),
            cv.Optional(CONF_BUTTON_STYLE_ID): cv.use_id(lv_style_t),
            cv.Optional(CONF_ACCENT_BUTTON_STYLE_ID): cv.use_id(lv_style_t),
            cv.Optional(CONF_FOCUS_STYLE_ID): cv.use_id(lv_style_t),
            cv.Optional(CONF_ON_EXIT): automation.validate_automation(single=True),
        }
    )(value)


async def _controls_to_code(player, conf, pictures, queue):
    cg.add_define("USE_MEDIA_PLAYER_CONTROLS")
    var = cg.Pvariable(conf[CONF_ID], cg.MockObj(str(MediaControls), "::").create())
    await cg.register_component(var, conf)
    cg.add(var.set_player(player))
    cg.add(var.set_lvgl(await cg.get_variable(conf[CONF_LVGL_ID])))
    if pictures is not None:
        cg.add(var.set_pictures(pictures))
    if queue is not None:
        cg.add(var.set_queue(queue))
    cg.add(var.set_hide_after(conf[CONF_HIDE_AFTER]))
    for button in conf[CONF_BUTTONS]:
        cg.add(var.add_button(CONTROL_BUTTONS[button]))
    cg.add(var.set_show_title(conf[CONF_TITLE]))
    for key, setter in (
        (CONF_BAR_STYLE_ID, "set_bar_style"),
        (CONF_BUTTON_STYLE_ID, "set_button_style"),
        (CONF_ACCENT_BUTTON_STYLE_ID, "set_accent_style"),
        (CONF_FOCUS_STYLE_ID, "set_focus_style"),
    ):
        if key in conf:
            cg.add(getattr(var, setter)(await cg.get_variable(conf[key])))
    if on_exit := conf.get(CONF_ON_EXIT):
        await automation.build_automation(var.get_exit_trigger(), [], on_exit)
CONF_IMAGE_ID = "image_id"
CONF_DIRECTORIES = "directories"
CONF_SLIDESHOW_INTERVAL = "slideshow_interval"
CONF_DEFAULT_IMAGE_INDEX = "default_image_index"
CONF_THUMBNAILS = "thumbnails"
CONF_ON_PICTURE = "on_picture"


def _storage_dir(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/"):
        raise cv.Invalid("a directory's storage path, as /sd/pictures")
    return value


def _pictures(value):
    from esphome.components.online_image.image import OnlineImage

    return cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MediaPictures),
            cv.Required(CONF_IMAGE_ID): cv.use_id(OnlineImage),
            cv.Optional(CONF_DIRECTORIES, default=[]): cv.ensure_list(_storage_dir),
            cv.Optional(CONF_SLIDESHOW_INTERVAL, default="5s"): cv.All(
                cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(milliseconds=500))
            ),
            cv.Optional(CONF_DEFAULT_IMAGE_INDEX, default=0): cv.Any(
                cv.int_range(min=0), cv.one_of("off", lower=True)
            ),
            cv.Optional(CONF_THUMBNAILS, default=[]): cv.All(cv.ensure_list(cv.use_id(OnlineImage)), cv.Length(max=50)),
            cv.Optional(CONF_ON_PICTURE): automation.validate_automation(single=True),
        }
    )(value)


async def _pictures_to_code(conf):
    cg.add_define("USE_MEDIA_PLAYER_PICTURES")
    var = cg.Pvariable(conf[CONF_ID], cg.MockObj(str(MediaPictures), "::").create())
    await cg.register_component(var, conf)
    cg.add(var.set_image(await cg.get_variable(conf[CONF_IMAGE_ID])))
    for directory in conf[CONF_DIRECTORIES]:
        cg.add(var.add_directory(directory))
    cg.add(var.set_slideshow_interval(conf[CONF_SLIDESHOW_INTERVAL]))
    index = conf[CONF_DEFAULT_IMAGE_INDEX]
    cg.add(var.set_default_index(-1 if index == "off" else index))
    for thumb in conf[CONF_THUMBNAILS]:
        cg.add(var.add_thumbnail(await cg.get_variable(thumb)))
    if on_picture := conf.get(CONF_ON_PICTURE):
        await automation.build_automation(var.get_picture_trigger(), [], on_picture)
    return var

CONF_EXTENDED_API_SERVICES = "extended_api_services"
CONF_COVER_CANVASES = "cover_canvases"
CONF_LVGL_ID = "lvgl_id"


def _extended_api_services(value):
    from esphome.components import text_sensor
    from esphome.components.lvgl.lvcode import LvglComponent
    from esphome.components.lvgl.widgets.canvas import lv_canvas_t

    if value is True or value == {}:
        value = {}
    elif value is False:
        return False
    schema = cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ExtendedApiServices),
            cv.Optional(CONF_STATUS, default={"name": "Media status"}): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_COVER_CANVASES): cv.All(cv.ensure_list(cv.use_id(lv_canvas_t)), cv.Length(min=1, max=3)),
            cv.Optional(CONF_LVGL_ID): cv.use_id(LvglComponent),
        }
    )
    value = schema(value)
    cv.requires_component("api")(value)
    cv.requires_component("text_sensor")(value)
    if CONF_COVER_CANVASES in value and CONF_LVGL_ID not in value:
        raise cv.Invalid(f"{CONF_COVER_CANVASES}: the canvases are LVGL's: set {CONF_LVGL_ID}")
    return value


async def _extended_api_services_to_code(player, conf):
    from esphome.components import text_sensor

    cg.add_define("USE_MEDIA_PLAYER_EXTENDED_API")
    var = cg.Pvariable(conf[CONF_ID], cg.MockObj(str(ExtendedApiServices), "::").create())
    await cg.register_component(var, conf)
    cg.add(var.set_media_player(player))
    cg.add(var.set_status(await text_sensor.new_text_sensor(conf[CONF_STATUS])))
    if CONF_COVER_CANVASES in conf:
        cg.add_define("USE_MEDIA_PLAYER_COVER")
        from esphome.components import relay_link

        cg.add(var.set_relay(await relay_link.get_relay_link()))
        cg.add(var.set_lvgl(await cg.get_variable(conf[CONF_LVGL_ID])))
        for canvas in conf[CONF_COVER_CANVASES]:
            cg.add(var.add_cover_canvas(await cg.get_variable(canvas)))

MediaPlayerSupportedFormat = media_player_ns.struct("MediaPlayerSupportedFormat")

MediaPlayerFormatPurpose = media_player_ns.enum(
    "MediaPlayerFormatPurpose", is_class=True
)
MEDIA_PLAYER_FORMAT_PURPOSE_ENUM = MediaPlayerFormatPurpose.options(
    "default", "announcement", prefix="PURPOSE_"
)

# Public API for external components. Do not remove.
FORMAT_MAPPING = {
    "FLAC": "flac",
    "MP3": "mp3",
    "OPUS": "opus",
    "WAV": "wav",
}


# MP3 constant bitrates in kbps; the encoder rounds anything else to the nearest one
MP3_BITRATES = (
    8,
    16,
    24,
    32,
    40,
    48,
    56,
    64,
    80,
    96,
    112,
    128,
    144,  # MPEG-2 only, at 16, 22.05 and 24 kHz
    160,
    192,
    224,
    256,
    320,
)
validate_mp3_bitrate = cv.one_of(*MP3_BITRATES, int=True)


def build_supported_format_struct(
    format_config: ConfigType, purpose: MockObj
) -> cg.StructInitializer:
    """Build a MediaPlayerSupportedFormat struct from a format config and purpose.

    Public API for external components. Do not remove.
    """
    args = [
        MediaPlayerSupportedFormat,
        ("format", FORMAT_MAPPING[format_config[CONF_FORMAT]]),
        ("sample_rate", format_config[CONF_SAMPLE_RATE]),
        ("num_channels", format_config[CONF_NUM_CHANNELS]),
        ("purpose", purpose),
    ]

    # Omit sample_bytes for MP3: ffmpeg transcoding in Home Assistant fails
    # if the number of bytes per sample is specified for MP3.
    if format_config[CONF_FORMAT] != "MP3":
        args.append(("sample_bytes", 2))

    # Omitted means no preference, and the client keeps encoding as it always has.
    if bitrate := format_config.get(CONF_BITRATE):
        args.append(("bitrate", bitrate))

    return cg.StructInitializer(*args)


def validate_preferred_format(
    component_name: str, audio_device_key: str
) -> Callable[[ConfigType], ConfigType]:
    """Return a validator that inherits audio device settings and validates format constraints.

    Public API for external components. Do not remove.
    """

    def validator(config: ConfigType) -> ConfigType:
        from esphome.components import audio

        # Inherit settings from audio device if not manually set
        if (device_id := config.get(audio_device_key)) is not None:
            audio.resolve_device_format(device_id)
        inherit_property_from(CONF_NUM_CHANNELS, audio_device_key)(config)
        inherit_property_from(CONF_SAMPLE_RATE, audio_device_key)(config)

        # A constant bitrate is only meaningful for the one lossy format whose
        # encoder Home Assistant can be told to fix
        if CONF_BITRATE in config and config.get(CONF_FORMAT) != "MP3":
            raise cv.Invalid(
                f"{CONF_BITRATE} is only supported for the MP3 format",
                path=[CONF_BITRATE],
            )

        # Opus only supports 48 kHz
        if config.get(CONF_FORMAT) == "OPUS" and config.get(CONF_SAMPLE_RATE) != 48000:
            raise cv.Invalid("Opus only supports a sample rate of 48000 Hz")

        # Validate the settings are compatible with the audio device
        audio.final_validate_audio_schema(
            component_name,
            audio_device=audio_device_key,
            bits_per_sample=16,
            channels=config.get(CONF_NUM_CHANNELS),
            sample_rate=config.get(CONF_SAMPLE_RATE),
        )(config)

        return config

    return validator


def request_codecs_for_format_configs(
    config: ConfigType, format_config_keys: list[str]
) -> None:
    """Scan format configs for configured formats and request the needed codec support.

    If any config uses "NONE" (accepts any format), all codecs are requested.

    Public API for external components. Do not remove.
    """
    from esphome.components import audio

    needed_formats: set[str] = set()
    need_all = False

    for key in format_config_keys:
        if format_config := config.get(key):
            fmt = format_config[CONF_FORMAT]
            if fmt == "NONE":
                need_all = True
            else:
                needed_formats.add(fmt)

    if need_all:
        audio.request_flac_support()
        audio.request_mp3_support()
        audio.request_opus_support()
        audio.request_wav_support()
    else:
        if "FLAC" in needed_formats:
            audio.request_flac_support()
        if "MP3" in needed_formats:
            audio.request_mp3_support()
        if "OPUS" in needed_formats:
            audio.request_opus_support()
        if "WAV" in needed_formats:
            audio.request_wav_support()


# Local config key constants
CONF_ANNOUNCEMENT = "announcement"
CONF_ON_PLAY = "on_play"
CONF_ON_PAUSE = "on_pause"
CONF_ON_ANNOUNCEMENT = "on_announcement"
CONF_MEDIA_URL = "media_url"

# Command actions that all share the same schema and only differ in the command sent
_COMMAND_ACTIONS = [
    "play",
    "pause",
    "stop",
    "toggle",
    "volume_up",
    "volume_down",
    "turn_on",
    "turn_off",
    "next",
    "previous",
    "mute",
    "unmute",
    "repeat_off",
    "repeat_one",
    "repeat_all",
    "shuffle",
    "unshuffle",
    "group_join",
    "clear_playlist",
]

StateAnyForwarder = media_player_ns.class_("StateAnyForwarder")
StateEnterForwarder = media_player_ns.class_("StateEnterForwarder")
MediaPlayerState = media_player_ns.enum("MediaPlayerState")

# State triggers: (config_key, state enum or None for any-state)
_STATE_TRIGGERS = (
    (CONF_ON_STATE, None),
    (CONF_ON_IDLE, MediaPlayerState.MEDIA_PLAYER_STATE_IDLE),
    (CONF_ON_PLAY, MediaPlayerState.MEDIA_PLAYER_STATE_PLAYING),
    (CONF_ON_PAUSE, MediaPlayerState.MEDIA_PLAYER_STATE_PAUSED),
    (CONF_ON_ANNOUNCEMENT, MediaPlayerState.MEDIA_PLAYER_STATE_ANNOUNCING),
    (CONF_ON_TURN_ON, MediaPlayerState.MEDIA_PLAYER_STATE_ON),
    (CONF_ON_TURN_OFF, MediaPlayerState.MEDIA_PLAYER_STATE_OFF),
)

# State conditions: (config_key suffix, checked state)
_STATE_CONDITIONS = (
    ("idle", MediaPlayerState.MEDIA_PLAYER_STATE_IDLE),
    ("paused", MediaPlayerState.MEDIA_PLAYER_STATE_PAUSED),
    ("playing", MediaPlayerState.MEDIA_PLAYER_STATE_PLAYING),
    ("announcing", MediaPlayerState.MEDIA_PLAYER_STATE_ANNOUNCING),
    ("on", MediaPlayerState.MEDIA_PLAYER_STATE_ON),
    ("off", MediaPlayerState.MEDIA_PLAYER_STATE_OFF),
)

MediaPlayerCommand = media_player_ns.enum("MediaPlayerCommand", is_class=True)


def _on_state_enter(conf_key: str, state: cg.MockObj) -> automation.CallbackAutomation:
    return automation.CallbackAutomation(
        conf_key,
        "add_on_state_callback",
        forwarder=StateEnterForwarder.template(state),
        params=[(MediaPlayerState, "state")],
        forward=[],
        when=f"state == {state}",
    )


_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(
        CONF_ON_STATE,
        "add_on_state_callback",
        forwarder=StateAnyForwarder,
        params=[(MediaPlayerState, "state")],
        forward=[],
    ),
    _on_state_enter(CONF_ON_IDLE, MediaPlayerState.MEDIA_PLAYER_STATE_IDLE),
    _on_state_enter(CONF_ON_PLAY, MediaPlayerState.MEDIA_PLAYER_STATE_PLAYING),
    _on_state_enter(CONF_ON_PAUSE, MediaPlayerState.MEDIA_PLAYER_STATE_PAUSED),
    _on_state_enter(
        CONF_ON_ANNOUNCEMENT, MediaPlayerState.MEDIA_PLAYER_STATE_ANNOUNCING
    ),
    _on_state_enter(CONF_ON_TURN_ON, MediaPlayerState.MEDIA_PLAYER_STATE_ON),
    _on_state_enter(CONF_ON_TURN_OFF, MediaPlayerState.MEDIA_PLAYER_STATE_OFF),
)


@setup_entity("media_player")
async def setup_media_player_core_(var, config):
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)
    if conf := config.get(CONF_EXTENDED_API_SERVICES):
        await _extended_api_services_to_code(var, conf)
    pictures = None
    if conf := config.get(CONF_PICTURES):
        pictures = await _pictures_to_code(conf)
    queue = None
    if conf := config.get(CONF_QUEUE):
        queue = await _queue_to_code(var, conf)
    if conf := config.get(CONF_CONTROLS):
        await _controls_to_code(var, conf, pictures, queue)


async def register_media_player(var, config):
    if not CORE.has_id(config[CONF_ID]):
        var = cg.Pvariable(config[CONF_ID], var)
    queue_entity_register("media_player", config)
    CORE.register_platform_component("media_player", var)
    await setup_media_player_core_(var, config)


async def new_media_player(config, *args):
    var = cg.new_Pvariable(config[CONF_ID], *args)
    await register_media_player(var, config)
    return var


_MEDIA_PLAYER_SCHEMA = cv.ENTITY_BASE_SCHEMA.extend(
    {
        **{cv.Optional(conf_key): automation.validate_automation({}) for conf_key, _ in _STATE_TRIGGERS},
        cv.Optional(CONF_EXTENDED_API_SERVICES): _extended_api_services,
        cv.Optional(CONF_PICTURES): _pictures,
        cv.Optional(CONF_CONTROLS): _controls,
        cv.Optional(CONF_QUEUE): QUEUE_SCHEMA,
    }
)

_MEDIA_PLAYER_SCHEMA.add_extra(entity_duplicate_validator("media_player"))


def media_player_schema(
    class_: MockObjClass,
    *,
    entity_category: str = cv.UNDEFINED,
    icon: str = cv.UNDEFINED,
) -> cv.Schema:
    schema = {cv.GenerateID(CONF_ID): cv.declare_id(class_)}

    for key, default, validator in [
        (CONF_ENTITY_CATEGORY, entity_category, cv.entity_category),
        (CONF_ICON, icon, cv.icon),
    ]:
        if default is not cv.UNDEFINED:
            schema[cv.Optional(key, default=default)] = validator

    return _MEDIA_PLAYER_SCHEMA.extend(schema)


MEDIA_PLAYER_ACTION_SCHEMA = automation.maybe_simple_id(
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(MediaPlayer),
            cv.Optional(CONF_ANNOUNCEMENT, default=False): cv.templatable(cv.boolean),
        }
    )
)

MEDIA_PLAYER_CONDITION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(MediaPlayer)}
)


_MEDIA_URL_ACTION_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(MediaPlayer),
        cv.Required(CONF_MEDIA_URL): cv.templatable(cv.url),
        cv.Optional(CONF_ANNOUNCEMENT, default=False): cv.templatable(cv.boolean),
    },
    key=CONF_MEDIA_URL,
)


_ANNOUNCEMENT_FIELD = automation.ApplyField(
    CONF_ANNOUNCEMENT, "set_announcement", cg.bool_
)
_MEDIA_URL_FIELD = automation.ApplyField(CONF_MEDIA_URL, "set_media_url", cg.std_string)


def _set_command(command_name: str) -> automation.ApplyCall:
    command = getattr(
        MediaPlayerCommand, f"MEDIA_PLAYER_COMMAND_{command_name.upper()}"
    )
    return automation.ApplyCall(f"set_command({command})")


automation.register_apply_action(
    "media_player.play_media",
    _MEDIA_URL_ACTION_SCHEMA,
    _MEDIA_URL_FIELD,
    _ANNOUNCEMENT_FIELD,
    call="make_call",
)

automation.register_apply_action(
    "media_player.enqueue",
    _MEDIA_URL_ACTION_SCHEMA,
    _set_command("enqueue"),
    _MEDIA_URL_FIELD,
    _ANNOUNCEMENT_FIELD,
    call="make_call",
)

for _action_name in _COMMAND_ACTIONS:
    automation.register_apply_action(
        f"media_player.{_action_name}",
        MEDIA_PLAYER_ACTION_SCHEMA,
        _set_command(_action_name),
        _ANNOUNCEMENT_FIELD,
        call="make_call",
    )


for _condition_name, _state in _STATE_CONDITIONS:
    automation.register_apply_condition(
        f"media_player.is_{_condition_name}",
        MEDIA_PLAYER_CONDITION_SCHEMA,
        f"state == {_state}",
    )

automation.register_apply_condition(
    "media_player.is_muted", MEDIA_PLAYER_CONDITION_SCHEMA, "is_muted()"
)


automation.register_apply_action(
    "media_player.volume_set",
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(MediaPlayer),
            cv.Required(CONF_VOLUME): cv.templatable(cv.percentage),
        },
        key=CONF_VOLUME,
    ),
    automation.ApplyField(CONF_VOLUME, "set_volume", cg.float_),
    call="make_call",
)


@coroutine_with_priority(CoroPriority.CORE)
async def to_code(config):
    cg.add_global(media_player_ns.using)
    cg.add_define("USE_MEDIA_PLAYER")


PICTURES_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(MediaPictures)})
PICTURES_COMMANDS = {
    "next": 0,
    "previous": 1,
    "slideshow_start": 2,
    "slideshow_stop": 3,
    "slideshow_toggle": 4,
    "thumbnails_next": 5,
    "thumbnails_previous": 6,
    "close": 7,
}


def _register_pictures_command(name, command):
    @automation.register_action(
        f"media_player.pictures.{name}", PicturesCommandAction, PICTURES_ACTION_SCHEMA, synchronous=True
    )
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_command(command))
        return var

    return _to_code


for _name, _command in PICTURES_COMMANDS.items():
    _register_pictures_command(_name, _command)


@automation.register_action(
    "media_player.pictures.show",
    PicturesShowAction,
    cv.All(
        cv.Schema(
            {
                cv.GenerateID(): cv.use_id(MediaPictures),
                cv.Exclusive("index", "picture"): cv.templatable(cv.int_range(min=0)),
                cv.Exclusive("path", "picture"): cv.templatable(cv.string),
            }
        ),
        cv.has_exactly_one_key("index", "path"),
    ),
    synchronous=True,
)
async def pictures_show_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if "path" in config:
        cg.add(var.set_by_path(True))
        cg.add(var.set_path(await cg.templatable(config["path"], args, cg.std_string)))
    else:
        cg.add(var.set_index(await cg.templatable(config["index"], args, cg.int_)))
    return var


@automation.register_action(
    "media_player.pictures.open_directory",
    PicturesOpenAction,
    cv.Schema({cv.GenerateID(): cv.use_id(MediaPictures), cv.Required("directory"): cv.templatable(cv.string)}),
    synchronous=True,
)
async def pictures_open_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_directory(await cg.templatable(config["directory"], args, cg.std_string)))
    return var


@automation.register_action(
    "media_player.pictures.set_interval",
    PicturesIntervalAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(MediaPictures),
            cv.Required("interval"): cv.templatable(cv.positive_time_period_milliseconds),
        }
    ),
    synchronous=True,
)
async def pictures_interval_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_interval(await cg.templatable(config["interval"], args, cg.uint32)))
    return var


@automation.register_action(
    "media_player.pictures.thumbnail_show",
    PicturesThumbnailAction,
    cv.Schema({cv.GenerateID(): cv.use_id(MediaPictures), cv.Required("slot"): cv.templatable(cv.int_range(min=0, max=49))}),
    synchronous=True,
)
async def pictures_thumbnail_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_slot(await cg.templatable(config["slot"], args, cg.int_)))
    return var


CONTROLS_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(MediaControls)})


def _register_controls(name, mode):
    @automation.register_action(f"media_player.controls.{name}", ControlsAction, CONTROLS_ACTION_SCHEMA, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_mode(mode))
        return var

    return _to_code


for _name, _mode in (("show", 0), ("hide", 1), ("toggle", 2)):
    _register_controls(_name, _mode)


@automation.register_action(
    "media_player.controls.set_title",
    ControlsTitleAction,
    cv.Schema({cv.GenerateID(): cv.use_id(MediaControls), cv.Required(CONF_TITLE): cv.templatable(cv.string)}),
    synchronous=True,
)
async def controls_title_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_title(await cg.templatable(config[CONF_TITLE], args, cg.std_string)))
    return var


QUEUE_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(MediaQueue)})


def _register_queue_command(name, command):
    @automation.register_action(f"media_player.queue.{name}", QueueCommandAction, QUEUE_ACTION_SCHEMA, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_command(command))
        return var

    return _to_code


for _name, _command in (("next", 0), ("previous", 1), ("play_or_resume", 2), ("stop", 3), ("clear", 4)):
    _register_queue_command(_name, _command)


@automation.register_action(
    "media_player.queue.add",
    QueueAddAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(MediaQueue),
            cv.Required("path"): cv.templatable(cv.string),
            cv.Optional("title", default=""): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def queue_add_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config["path"], args, cg.std_string)))
    cg.add(var.set_title(await cg.templatable(config["title"], args, cg.std_string)))
    return var


@automation.register_action(
    "media_player.queue.play",
    QueuePlayAction,
    cv.Schema({cv.GenerateID(): cv.use_id(MediaQueue), cv.Required("index"): cv.templatable(cv.int_range(min=0))}),
    synchronous=True,
)
async def queue_play_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_index(await cg.templatable(config["index"], args, cg.int_)))
    return var


@automation.register_action(
    "media_player.queue.set_shuffle",
    QueueModeAction,
    cv.Schema({cv.GenerateID(): cv.use_id(MediaQueue), cv.Required("shuffle"): cv.templatable(cv.boolean)}),
    synchronous=True,
)
async def queue_shuffle_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_kind(0))
    cg.add(var.set_shuffle(await cg.templatable(config["shuffle"], args, cg.bool_)))
    return var


@automation.register_action(
    "media_player.queue.set_repeat",
    QueueModeAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(MediaQueue),
            cv.Required("repeat"): cv.templatable(cv.enum({"off": 0, "one": 1, "all": 2}, lower=True)),
        }
    ),
    synchronous=True,
)
async def queue_repeat_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_kind(1))
    cg.add(var.set_repeat(await cg.templatable(config["repeat"], args, cg.int_)))
    return var
