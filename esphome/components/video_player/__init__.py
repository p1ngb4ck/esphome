"""Video player component for ESP32-P4 with H264 hardware decoding."""

from esphome import automation
import esphome.codegen as cg

# Register H264 decoder requirement with transcoder
from esphome.components import transcoder
from esphome.components.lvgl.widgets.canvas import lv_canvas_t
import esphome.config_validation as cv
from esphome.const import CONF_FILE, CONF_ID
from esphome.core import CORE

transcoder.require_h264_decoder()

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["lvgl", "transcoder"]
AUTO_LOAD = ["audio"]

# ESP32-P4 only (hardware H264 decoder)
CONFLICTS_WITH = []

video_player_ns = cg.esphome_ns.namespace("video_player")
VideoPlayer = video_player_ns.class_("VideoPlayer", cg.Component)

# Actions
PlayAction = video_player_ns.class_("PlayAction", automation.Action)
PauseAction = video_player_ns.class_("PauseAction", automation.Action)
StopAction = video_player_ns.class_("StopAction", automation.Action)
SeekAction = video_player_ns.class_("SeekAction", automation.Action)

# Triggers
PlaybackStartedTrigger = video_player_ns.class_(
    "PlaybackStartedTrigger", automation.Trigger.template()
)
PlaybackPausedTrigger = video_player_ns.class_(
    "PlaybackPausedTrigger", automation.Trigger.template()
)
PlaybackStoppedTrigger = video_player_ns.class_(
    "PlaybackStoppedTrigger", automation.Trigger.template()
)
PlaybackFinishedTrigger = video_player_ns.class_(
    "PlaybackFinishedTrigger", automation.Trigger.template()
)

# Configuration keys
CONF_VIDEO_FILE = "video_file"
CONF_CANVAS = "canvas"
CONF_AUTO_PLAY = "auto_play"
CONF_LOOP = "loop"
CONF_ON_PLAYBACK_STARTED = "on_playback_started"
CONF_ON_PLAYBACK_PAUSED = "on_playback_paused"
CONF_ON_PLAYBACK_STOPPED = "on_playback_stopped"
CONF_ON_PLAYBACK_FINISHED = "on_playback_finished"

CONF_SPEAKER = "speaker"
CONF_CONTAINERS = "containers"
CONF_AUDIO_CODECS = "audio_codecs"

# Container and codec types
CONTAINER_TYPES = ["mp4", "mkv"]
AUDIO_CODEC_TYPES = ["aac", "mp3", "flac"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VideoPlayer),
        cv.Optional(CONF_CANVAS): cv.use_id(
            lv_canvas_t
        ),  # Now optional for audio-only playback
        cv.Optional(CONF_SPEAKER): cv.use_id(
            cg.ComponentPtr.template(
                cg.esphome_ns.namespace("speaker").class_("Speaker")
            )
        ),
        # Container and codec selection
        cv.Optional(CONF_CONTAINERS, default=["mp4"]): cv.ensure_list(
            cv.one_of(*CONTAINER_TYPES, lower=True)
        ),
        cv.Optional(CONF_AUDIO_CODECS, default=["mp3", "flac"]): cv.ensure_list(
            cv.one_of(*AUDIO_CODEC_TYPES, lower=True)
        ),
        cv.Optional(CONF_VIDEO_FILE): cv.string,
        cv.Optional(CONF_AUTO_PLAY, default=False): cv.boolean,
        cv.Optional(CONF_LOOP, default=False): cv.boolean,
        cv.Optional(CONF_ON_PLAYBACK_STARTED): automation.validate_automation(
            {
                cv.GenerateID(automation.CONF_TRIGGER_ID): cv.declare_id(
                    PlaybackStartedTrigger
                ),
            }
        ),
        cv.Optional(CONF_ON_PLAYBACK_PAUSED): automation.validate_automation(
            {
                cv.GenerateID(automation.CONF_TRIGGER_ID): cv.declare_id(
                    PlaybackPausedTrigger
                ),
            }
        ),
        cv.Optional(CONF_ON_PLAYBACK_STOPPED): automation.validate_automation(
            {
                cv.GenerateID(automation.CONF_TRIGGER_ID): cv.declare_id(
                    PlaybackStoppedTrigger
                ),
            }
        ),
        cv.Optional(CONF_ON_PLAYBACK_FINISHED): automation.validate_automation(
            {
                cv.GenerateID(automation.CONF_TRIGGER_ID): cv.declare_id(
                    PlaybackFinishedTrigger
                ),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def validate_video_player(config):
    """Validate video player configuration."""
    # Ensure we're on ESP32-P4
    if not CORE.is_esp32:
        raise cv.Invalid("video_player only supports ESP32 platform")

    import logging

    from esphome.components.esp32 import get_esp32_variant

    _LOGGER = logging.getLogger(__name__)

    variant = get_esp32_variant()
    if "P4" not in variant:
        raise cv.Invalid(
            f"video_player requires ESP32-P4 for hardware H264 decoder (current: {variant})"
        )

    # Canvas required only if video playback needed (not audio-only)
    if CONF_CANVAS not in config and CONF_VIDEO_FILE in config:
        _LOGGER.warning("Canvas not configured - only audio playback will be available")

    # Speaker required when audio codecs are configured
    if config[CONF_AUDIO_CODECS] and CONF_SPEAKER not in config:
        raise cv.Invalid("Speaker required when audio codecs are configured")

    # Add audio codec defines EARLY (during validation, before component loading)
    # This ensures the audio component headers are compiled with these defines
    for codec in config[CONF_AUDIO_CODECS]:
        if codec == "aac":
            cg.add_define("USE_AAC_DECODER")
            cg.add_define("USE_AUDIO_AAC_SUPPORT", True)
        elif codec == "mp3":
            cg.add_define("USE_AUDIO_MP3_SUPPORT", True)
        elif codec == "flac":
            cg.add_define("USE_AUDIO_FLAC_SUPPORT", True)

    return config


FINAL_VALIDATE_SCHEMA = validate_video_player


async def to_code(config):
    """Generate C++ code for video player component."""
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Generate container defines
    for container in config[CONF_CONTAINERS]:
        cg.add_define(f"USE_{container.upper()}_CONTAINER")

    # Add audio codec defines and ESP-IDF components
    # Note: Defines are also added during validation, but we add them here too
    # to ensure they're in the generated defines.h during compilation
    for codec in config[CONF_AUDIO_CODECS]:
        if codec == "aac":
            cg.add_define("USE_AAC_DECODER")
            cg.add_define("USE_AUDIO_AAC_SUPPORT", True)
            # Add esp_audio_codec component for AAC support (ESP-IDF only)
            if CORE.using_esp_idf:
                from esphome.components.esp32 import add_idf_component

                add_idf_component(name="espressif/esp_audio_codec", ref="2.3.0")
        elif codec == "mp3":
            cg.add_define("USE_AUDIO_MP3_SUPPORT", True)
        elif codec == "flac":
            cg.add_define("USE_AUDIO_FLAC_SUPPORT", True)

    # Link to LVGL canvas (optional for audio-only)
    if CONF_CANVAS in config:
        canvas = await cg.get_variable(config[CONF_CANVAS])
        cg.add(var.set_canvas(canvas))

    # Link to speaker (optional for audio playback)
    if CONF_SPEAKER in config:
        speaker = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(speaker))

    # Configuration
    if CONF_VIDEO_FILE in config:
        cg.add(var.set_video_file(config[CONF_VIDEO_FILE]))
    cg.add(var.set_auto_play(config[CONF_AUTO_PLAY]))
    cg.add(var.set_loop(config[CONF_LOOP]))

    # Automation triggers
    for conf in config.get(CONF_ON_PLAYBACK_STARTED, []):
        trigger = cg.new_Pvariable(conf[automation.CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_PLAYBACK_PAUSED, []):
        trigger = cg.new_Pvariable(conf[automation.CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_PLAYBACK_STOPPED, []):
        trigger = cg.new_Pvariable(conf[automation.CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_PLAYBACK_FINISHED, []):
        trigger = cg.new_Pvariable(conf[automation.CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


# ========== Actions ==========


@automation.register_action(
    "video_player.play",
    PlayAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(VideoPlayer),
            cv.Optional(CONF_FILE): cv.templatable(cv.string),
        }
    ),
)
async def video_player_play_to_code(config, action_id, template_arg, args):
    """Register play action."""
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)

    if CONF_FILE in config:
        template_ = await cg.templatable(config[CONF_FILE], args, cg.std_string)
        cg.add(var.set_file(template_))

    return var


@automation.register_action(
    "video_player.pause",
    PauseAction,
    automation.maybe_simple_id(
        {
            cv.GenerateID(): cv.use_id(VideoPlayer),
        }
    ),
)
async def video_player_pause_to_code(config, action_id, template_arg, args):
    """Register pause action."""
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "video_player.stop",
    StopAction,
    automation.maybe_simple_id(
        {
            cv.GenerateID(): cv.use_id(VideoPlayer),
        }
    ),
)
async def video_player_stop_to_code(config, action_id, template_arg, args):
    """Register stop action."""
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "video_player.seek",
    SeekAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(VideoPlayer),
            cv.Required("position"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        }
    ),
)
async def video_player_seek_to_code(config, action_id, template_arg, args):
    """Register seek action."""
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)

    template_ = await cg.templatable(config["position"], args, cg.uint32)
    cg.add(var.set_position(template_))

    return var
