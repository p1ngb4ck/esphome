import esphome.codegen as cg
from esphome.components import audio, display, esp32, media_player, speaker
from esphome.components.esp32 import VARIANT_ESP32P4, only_on_variant
from esphome.components.lvgl.video_output import (
    VIDEO_OUTPUT_SCHEMA,
    final_validate_video_output,
    video_output_to_code,
)
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_DISPLAY_ID, CONF_SPEAKER
from esphome.types import ConfigType

from .. import avi_ns

DEPENDENCIES = ["esp32", "psram", "storage"]
AUTO_LOAD = ["avi"]

CONF_FRAME_SIZE = "frame_size"
CONF_AUDIO_CODECS = "audio_codecs"
CONF_AUDIO_LATENCY = "audio_latency"

AviMediaPlayer = avi_ns.class_("AviMediaPlayer", cg.Component, media_player.MediaPlayer)

CONFIG_SCHEMA = cv.All(
    media_player.media_player_schema(AviMediaPlayer).extend(
        {
            cv.Required(CONF_DISPLAY_ID): cv.use_id(display.Display),
            cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_BUFFER_SIZE, default="2MB"): cv.All(
                cv.validate_bytes, cv.Range(min=256 * 1024, max=16 * 1024 * 1024)
            ),
            cv.Optional(CONF_FRAME_SIZE, default="512kB"): cv.All(
                cv.validate_bytes, cv.Range(min=64 * 1024, max=2 * 1024 * 1024)
            ),
            cv.Optional(CONF_AUDIO_CODECS, default=["pcm"]): cv.ensure_list(
                cv.one_of("pcm", "mp3", "flac", lower=True)
            ),
            cv.Optional(CONF_AUDIO_LATENCY, default="0ms"): cv.Any(
                cv.returning_lambda,
                cv.All(cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(milliseconds=2000))),
            ),
        }
    )
    .extend(VIDEO_OUTPUT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32P4]),
)

FINAL_VALIDATE_SCHEMA = final_validate_video_output


async def to_code(config: ConfigType) -> None:
    var = await media_player.new_media_player(config)
    await cg.register_component(var, config)
    esp32.require_hw_jpeg()
    cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    await video_output_to_code(var, config)
    if CONF_SPEAKER in config:
        cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))
        if "mp3" in config[CONF_AUDIO_CODECS]:
            audio.request_mp3_support()
        if "flac" in config[CONF_AUDIO_CODECS]:
            audio.request_flac_support()
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(var.set_frame_size(config[CONF_FRAME_SIZE]))
    latency = config[CONF_AUDIO_LATENCY]
    if isinstance(latency, cv.Lambda):
        cg.add(var.set_audio_latency_lambda(await cg.process_lambda(latency, [], return_type=cg.uint32)))
    else:
        cg.add(var.set_audio_latency(latency.total_milliseconds))
