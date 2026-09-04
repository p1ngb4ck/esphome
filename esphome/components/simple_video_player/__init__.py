from __future__ import annotations

import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components import speaker
from esphome.components.audio import CONF_CODECS, CONF_FLAC, CONF_MP3
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_ID, CONF_TRIGGER_ID
from esphome.core import CORE
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

# Import LVGL canvas type for proper widget ID validation
try:
    from esphome.components.lvgl.widgets.canvas import lv_canvas_t

    LVGL_AVAILABLE = True
except ImportError:
    LVGL_AVAILABLE = False
    lv_canvas_t = None

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage"]
AUTO_LOAD = ["image", "audio"]

# Namespaces
simple_video_player_ns = cg.esphome_ns.namespace("simple_video_player")

# Classes
SimpleVideoPlayer = simple_video_player_ns.class_("SimpleVideoPlayer", cg.Component)

# Enums for speaker channel configuration
SpeakerChannelMode = simple_video_player_ns.enum("SpeakerChannelMode")
SPEAKER_CHANNEL_MODES = {
    "mono": SpeakerChannelMode.SPEAKER_CHANNEL_MONO,
    "left": SpeakerChannelMode.SPEAKER_CHANNEL_LEFT,
    "right": SpeakerChannelMode.SPEAKER_CHANNEL_RIGHT,
    "stereo": SpeakerChannelMode.SPEAKER_CHANNEL_STEREO,
}

# Automation triggers
PlaybackStartedTrigger = simple_video_player_ns.class_(
    "PlaybackStartedTrigger", automation.Trigger.template()
)
PlaybackFinishedTrigger = simple_video_player_ns.class_(
    "PlaybackFinishedTrigger", automation.Trigger.template()
)
PlaybackPausedTrigger = simple_video_player_ns.class_(
    "PlaybackPausedTrigger", automation.Trigger.template()
)
PlaybackErrorTrigger = simple_video_player_ns.class_(
    "PlaybackErrorTrigger", automation.Trigger.template(cg.uint8)
)

# Automation actions
PlayAction = simple_video_player_ns.class_("PlayAction", automation.Action)
PauseAction = simple_video_player_ns.class_("PauseAction", automation.Action)
ResumeAction = simple_video_player_ns.class_("ResumeAction", automation.Action)
StopAction = simple_video_player_ns.class_("StopAction", automation.Action)

# Configuration keys
CONF_CANVAS_ID = "canvas_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_CACHE_BUFFER_SIZE = "cache_buffer_size"
CONF_INPUT_BUFFER_SIZE = "input_buffer_size"
CONF_PREFETCH_FRAMES = "prefetch_frames"
CONF_TARGET_FPS = "target_fps"
CONF_ON_PLAYBACK_STARTED = "on_playback_started"
CONF_ON_PLAYBACK_FINISHED = "on_playback_finished"
CONF_ON_PLAYBACK_PAUSED = "on_playback_paused"
CONF_ON_PLAYBACK_ERROR = "on_playback_error"

# Default values
DEFAULT_CACHE_BUFFER_SIZE = 64 * 1024  # 64KB - optimized for better I/O performance
DEFAULT_INPUT_BUFFER_SIZE = 256 * 1024  # 256KB (per frame-ring-buffer slot)
# Depth of the video frame ring buffer (loader task prefetch, Core 0 -> decode task, Core 1).
# Generous by design: this pipeline currently targets ESP32-P4 (32MB PSRAM) with the hardware
# JPEG decoder; a size-conscious default for smaller/software-JPEG targets is future work.
DEFAULT_PREFETCH_FRAMES = 8
DEFAULT_TARGET_FPS = 30.0

# Validation ranges
MIN_CACHE_BUFFER_SIZE = 8 * 1024  # 8KB
MAX_CACHE_BUFFER_SIZE = 128 * 1024  # 128KB - increased for performance
MIN_INPUT_BUFFER_SIZE = 128 * 1024  # 128KB
MAX_INPUT_BUFFER_SIZE = 2 * 1024 * 1024  # 2MB
MIN_PREFETCH_FRAMES = 1
MAX_PREFETCH_FRAMES = 32
MIN_FPS = 1.0
MAX_FPS = 60.0


# Component configuration
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SimpleVideoPlayer),
            cv.Required(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
            cv.Optional(
                CONF_CACHE_BUFFER_SIZE, default=DEFAULT_CACHE_BUFFER_SIZE
            ): cv.int_range(min=MIN_CACHE_BUFFER_SIZE, max=MAX_CACHE_BUFFER_SIZE),
            cv.Optional(
                CONF_INPUT_BUFFER_SIZE, default=DEFAULT_INPUT_BUFFER_SIZE
            ): cv.int_range(min=MIN_INPUT_BUFFER_SIZE, max=MAX_INPUT_BUFFER_SIZE),
            cv.Optional(
                CONF_PREFETCH_FRAMES, default=DEFAULT_PREFETCH_FRAMES
            ): cv.int_range(min=MIN_PREFETCH_FRAMES, max=MAX_PREFETCH_FRAMES),
            cv.Optional(CONF_TARGET_FPS, default=DEFAULT_TARGET_FPS): cv.float_range(
                min=MIN_FPS, max=MAX_FPS
            ),
            # Automation triggers
            cv.Optional(CONF_ON_PLAYBACK_STARTED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        PlaybackStartedTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_PLAYBACK_FINISHED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        PlaybackFinishedTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_PLAYBACK_PAUSED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        PlaybackPausedTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_PLAYBACK_ERROR): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PlaybackErrorTrigger),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


def _final_validate(config):
    # Codec support (FLAC/MP3) is enabled by the user's own `audio: codecs:` block, never by
    # simple_video_player itself -- it only checks what's already there and warns. AVIAudioCodec
    # (avi_parser.h) only ever reports PCM, MP3 or FLAC for an AVI audio track -- WAV isn't a
    # thing that can occur there (it's a container format, not a distinct codec an AVI stream
    # carries), so it's not part of this check. Raw PCM needs no decoder at all either.
    if CONF_SPEAKER_ID not in config:
        return config

    full_config = fv.full_config.get()
    audio_config = full_config.get("audio")
    if isinstance(audio_config, list):
        # Defensive: audio is documented as single-instance, but don't assume forever.
        audio_config = audio_config[0] if audio_config else None

    enabled = set()
    if isinstance(audio_config, dict):
        codecs_config = audio_config.get(CONF_CODECS)
        if isinstance(codecs_config, dict):
            for name, key in (("flac", CONF_FLAC), ("mp3", CONF_MP3)):
                if key in codecs_config:
                    enabled.add(name)

    missing = {"flac", "mp3"} - enabled
    if missing:
        missing_str = ", ".join(sorted(missing))
        _LOGGER.warning(
            "simple_video_player: an AVI video's audio track can use FLAC, MP3 or raw PCM. "
            "%s not enabled under `audio: codecs:`. A video whose audio track uses one of those "
            "will play back with video only (no audio) at runtime -- add it there if your video "
            "files may use it.",
            f"{missing_str} {'is' if len(missing) == 1 else 'are'}",
        )

    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    # Backend selection (USE_HWJPG / USE_NEWJPEG / USE_JPEGDEC) is esp32's job - the single
    # source of truth also used by runtime_image.
    if CORE.is_esp32:
        from esphome.components.esp32 import require_hw_jpeg

        require_hw_jpeg()

    # File I/O streams through storage::StorageWorker (see buffered_file_reader.h) rather than a
    # blocking main-loop read; request it directly instead of relying on whichever storage
    # device the user happened to configure to have already asked for it.
    request_storage_worker()

    cg.add_define("USE_STORAGE")
    cg.add_define("USE_LVGL")

    # Get the single LVGL component instance (required for VSYNC callbacks)
    lvgl_configs = CORE.config.get("lvgl", [])
    if not lvgl_configs:
        raise cv.Invalid("LVGL component is required for simple_video_player")
    lvgl_id = lvgl_configs[0][CONF_ID]
    lvgl_component = await cg.get_variable(lvgl_id)

    var = cg.new_Pvariable(config[CONF_ID], lvgl_component)
    await cg.register_component(var, config)

    # Set canvas
    canvas = await cg.get_variable(config[CONF_CANVAS_ID])
    cg.add(var.set_canvas(canvas))

    # Set speaker (optional - for audio playback)
    if CONF_SPEAKER_ID in config:
        spkr = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_speaker(spkr))
        cg.add_define("USE_SPEAKER")
        cg.add_define("USE_AUDIO")

        # Extract speaker's channel configuration to enable proper audio routing
        # We need to look up the speaker's config to determine its channel mode
        speaker_config = CORE.config.get(CONF_SPEAKER_ID)
        if speaker_config and CONF_CHANNEL in speaker_config:
            channel_mode = speaker_config[CONF_CHANNEL]
            if channel_mode in SPEAKER_CHANNEL_MODES:
                cg.add(
                    var.set_speaker_channel_mode(SPEAKER_CHANNEL_MODES[channel_mode])
                )

    # Set buffer sizes
    cg.add(var.set_cache_buffer_size(config[CONF_CACHE_BUFFER_SIZE]))
    cg.add(var.set_input_buffer_size(config[CONF_INPUT_BUFFER_SIZE]))
    cg.add(var.set_prefetch_frames(config[CONF_PREFETCH_FRAMES]))

    # Set target FPS
    cg.add(var.set_target_fps(config[CONF_TARGET_FPS]))

    # Register automation triggers
    for conf in config.get(CONF_ON_PLAYBACK_STARTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_PLAYBACK_FINISHED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_PLAYBACK_PAUSED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_PLAYBACK_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.uint8, "error")], conf)


# Automation actions
SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
    }
)


@automation.register_action(
    "simple_video_player.play",
    PlayAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Required("path"): cv.templatable(cv.string),
        }
    ),
)
async def simple_video_player_play_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    template_ = await cg.templatable(config["path"], args, cg.std_string)
    cg.add(var.set_path(template_))
    return var


@automation.register_action(
    "simple_video_player.pause",
    PauseAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
)
async def simple_video_player_pause_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.resume",
    ResumeAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
)
async def simple_video_player_resume_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.stop",
    StopAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
)
async def simple_video_player_stop_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
