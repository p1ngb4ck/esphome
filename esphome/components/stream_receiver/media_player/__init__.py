from esphome import automation
import esphome.codegen as cg
from esphome.components import media_player, speaker, touchscreen
from esphome.components.esp32 import VARIANT_ESP32P4, add_idf_sdkconfig_option, only_on_variant
from esphome.components.lvgl.defines import CONF_LVGL_ID
from esphome.components.lvgl.widgets.canvas import lv_canvas_t
from esphome.components.lvgl.video_output import (
    CONF_CANVASES,
    CONF_VIDEO_OUTPUT,
    VIDEO_OUTPUT_SCHEMA,
    final_validate_video_output,
    video_output_to_code,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_CHANNELS,
    CONF_HEIGHT,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_PLATFORM,
    CONF_PORT,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
    CONF_WIDTH,
)
import esphome.final_validate as fv
from esphome.types import ConfigType

from .. import stream_receiver_ns

DEPENDENCIES = ["esp32", "psram", "network"]
CONF_WIDGET_ID = "widget_id"


def AUTO_LOAD(config=None) -> list[str]:
    if config and CONF_CHANNELS in config:
        return ["relay_link"]
    return []

CONF_MAX_FRAME_SIZE = "max_frame_size"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_AVI_PLAYER = "avi_player"
CONF_SOUND = "sound"
CONF_RESAMPLER = "resampler"
CONF_CODECS = "codecs"
CONF_CANVAS_ID = "canvas_id"
CONF_CANVAS_PAGE = "canvas_page"

StreamMediaPlayer = stream_receiver_ns.class_("StreamMediaPlayer", cg.Component, media_player.MediaPlayer)
AviMediaPlayer = cg.esphome_ns.namespace("avi").class_("AviMediaPlayer", cg.Component, media_player.MediaPlayer)
ChannelsAction = stream_receiver_ns.class_("ChannelsAction", automation.Action)
OutputAction = stream_receiver_ns.class_("OutputAction", automation.Action)


def _channels(value):
    from esphome.components.lvgl.widgets.dropdown import lv_dropdown_t

    return cv.Schema({cv.Optional(CONF_WIDGET_ID): cv.use_id(lv_dropdown_t)})(value or {})

CONFIG_SCHEMA = cv.All(
    media_player.media_player_schema(StreamMediaPlayer)
    .extend(
        {
            cv.Required(CONF_PORT): cv.port,
            cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(touchscreen.Touchscreen),
            cv.Optional(CONF_AVI_PLAYER): cv.use_id(AviMediaPlayer),
            cv.Optional(CONF_MAX_FRAME_SIZE, default="128kB"): cv.All(
                cv.validate_bytes, cv.Range(min=16 * 1024, max=1024 * 1024)
            ),
            cv.Optional(CONF_CHANNELS): _channels,
            cv.Optional(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
        }
    )
    .extend(VIDEO_OUTPUT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(CONF_LVGL_ID),
    only_on_variant(supported=[VARIANT_ESP32P4]),
)

CODEC_BITS = {"pcm": 1, "mp3": 2, "flac": 4}


def _final_validate(config: ConfigType) -> ConfigType:
    config = final_validate_video_output(config)
    fconf = fv.full_config.get()
    own = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_ID])[:-1])
    if CONF_SPEAKER in config:
        speaker_conf = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_SPEAKER])[:-1])
        resampler = speaker_conf.get(CONF_PLATFORM) == "resampler"
        rate = speaker_conf.get(CONF_SAMPLE_RATE, 48000)
        channels = speaker_conf.get(CONF_NUM_CHANNELS)
        if channels is None:
            channels = 1 if speaker_conf.get("channel") in ("mono", "left", "right") else 2
        own[CONF_SOUND] = (int(rate), min(int(channels), 2))
        own[CONF_RESAMPLER] = resampler
    codecs = 1
    if CONF_AVI_PLAYER in config:
        avi_conf = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_AVI_PLAYER])[:-1])
        codecs = 0
        for codec in avi_conf.get("audio_codecs", ["pcm"]):
            codecs |= CODEC_BITS.get(codec, 0)
    own[CONF_CODECS] = codecs
    if CONF_CANVAS_ID in config:
        lvgl = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_LVGL_ID])[:-1])
        canvases = [str(c) for c in lvgl[CONF_VIDEO_OUTPUT][CONF_CANVASES]]
        if str(config[CONF_CANVAS_ID]) not in canvases:
            raise cv.Invalid(f"canvas '{config[CONF_CANVAS_ID]}' is not one of the lvgl video_output canvases")
        canvas = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_CANVAS_ID])[:-1])
        width, height = canvas.get(CONF_WIDTH), canvas.get(CONF_HEIGHT)
        if not isinstance(width, int) or not isinstance(height, int):
            raise cv.Invalid(f"canvas '{config[CONF_CANVAS_ID]}' needs a pixel width and height")
        own[CONF_CANVAS_PAGE] = (canvases.index(str(config[CONF_CANVAS_ID])), width, height)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    var = await media_player.new_media_player(config)
    await cg.register_component(var, config)
    add_idf_sdkconfig_option("CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM", True)
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_frame_size(config[CONF_MAX_FRAME_SIZE]))
    cg.add(var.set_codecs(config[CONF_CODECS]))
    await video_output_to_code(var, config)
    if CONF_SPEAKER in config:
        spkr = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(spkr))
        rate, channels = config[CONF_SOUND]
        cg.add(var.set_sound(rate, channels))
        if config[CONF_RESAMPLER]:
            cg.add_define("USE_STREAM_RESAMPLER")
            cg.add(var.set_resampler(spkr))
    if CONF_TOUCHSCREEN_ID in config:
        cg.add_define("USE_STREAM_TOUCH")
        cg.add(var.set_touchscreen(await cg.get_variable(config[CONF_TOUCHSCREEN_ID])))
    if CONF_AVI_PLAYER in config:
        cg.add_define("USE_STREAM_AVI")
        cg.add(var.set_avi_player(await cg.get_variable(config[CONF_AVI_PLAYER])))
    if (canvas_page := config.get(CONF_CANVAS_PAGE)) is not None:
        cg.add_define("USE_STREAM_CANVAS")
        cg.add(var.set_canvas_page(*canvas_page))
    if (channels := config.get(CONF_CHANNELS)) is not None:
        from esphome.components import relay_link

        cg.add_define("USE_STREAM_CHANNELS")
        cg.add_define("USE_RELAY_LINK_CHANNELS")
        relay = await relay_link.get_relay_link()
        cg.add(relay.set_stream_port(config[CONF_PORT]))
        if CONF_WIDGET_ID in channels:
            cg.add_define("USE_RELAY_LINK_WIDGET")
            cg.add(relay.set_channel_widget(await cg.get_variable(channels[CONF_WIDGET_ID])))
            cg.add(relay.set_lvgl(await cg.get_variable(config[CONF_LVGL_ID])))
    try:
        from esphome.components import network

        network.require_high_performance_networking()
    except (ImportError, AttributeError):
        pass
    try:
        from esphome.components import wifi
    except ImportError:
        wifi = None
    for enable in ("enable_runtime_roaming_suppression", "enable_runtime_power_save_control"):
        request = getattr(wifi, enable, None)
        if request is not None:
            request()


CHANNELS_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(StreamMediaPlayer)})


@automation.register_action("media_player.channels.refresh", ChannelsAction, CHANNELS_ACTION_SCHEMA, synchronous=True)
async def channels_refresh_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_mode(0))
    return var


@automation.register_action("media_player.channels.stop", ChannelsAction, CHANNELS_ACTION_SCHEMA, synchronous=True)
async def channels_stop_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_mode(2))
    return var


@automation.register_action(
    "media_player.channels.play",
    ChannelsAction,
    cv.Schema({cv.GenerateID(): cv.use_id(StreamMediaPlayer), cv.Required("index"): cv.templatable(cv.int_range(min=0))}),
    synchronous=True,
)
async def channels_play_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_mode(1))
    cg.add(var.set_index(await cg.templatable(config["index"], args, cg.int_)))
    return var


@automation.register_action(
    "media_player.stream.output",
    OutputAction,
    cv.Schema({cv.GenerateID(): cv.use_id(StreamMediaPlayer), cv.Required("fullscreen"): cv.templatable(cv.boolean)}),
    synchronous=True,
)
async def stream_output_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_fullscreen(await cg.templatable(config["fullscreen"], args, cg.bool_)))
    return var
