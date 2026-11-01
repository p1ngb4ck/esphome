import esphome.codegen as cg
from esphome.components import media_player, speaker, touchscreen
from esphome.components.esp32 import VARIANT_ESP32P4, add_idf_sdkconfig_option, only_on_variant
from esphome.components.lvgl.defines import CONF_LVGL_ID
from esphome.components.lvgl.video_output import (
    VIDEO_OUTPUT_SCHEMA,
    final_validate_video_output,
    video_output_to_code,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_PLATFORM,
    CONF_PORT,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)
import esphome.final_validate as fv
from esphome.types import ConfigType

from .. import stream_receiver_ns

DEPENDENCIES = ["esp32", "psram", "network"]

CONF_MAX_FRAME_SIZE = "max_frame_size"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_AVI_PLAYER = "avi_player"
CONF_SOUND = "sound"
CONF_RESAMPLER = "resampler"
CONF_CODECS = "codecs"

StreamMediaPlayer = stream_receiver_ns.class_("StreamMediaPlayer", cg.Component, media_player.MediaPlayer)
AviMediaPlayer = cg.esphome_ns.namespace("avi").class_("AviMediaPlayer", cg.Component, media_player.MediaPlayer)

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
