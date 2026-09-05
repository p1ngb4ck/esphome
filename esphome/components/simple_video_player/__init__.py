from __future__ import annotations

from esphome import automation
import esphome.codegen as cg
from esphome.components import speaker
from esphome.components.audio import CONF_CODECS, CONF_FLAC, CONF_MP3
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_CHANNEL,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE
import esphome.final_validate as fv

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
CONF_AUDIO_CODEC = "audio_codec"
# Internal-only keys (never part of CONFIG_SCHEMA): _final_validate resolves these from the
# referenced speaker's own config and stashes them here for to_code() to read back.
CONF_AUDIO_SAMPLE_RATE = "audio_sample_rate"
CONF_AUDIO_CHANNELS = "audio_channels"
CONF_AUDIO_BITS_PER_SAMPLE = "audio_bits_per_sample"
CONF_RESOLVED_SPEAKER_CHANNEL = "resolved_speaker_channel"

# This is an MCU: rather than auto-detecting and reconfiguring buffers per video file (real heap
# allocation, every play() -- see AGENTS.md), the user commits to ONE fixed audio format up front.
# sample_rate/channels/bits_per_sample are NOT separate simple_video_player options -- they're
# properties of the speaker hardware the user already configured under `speaker_id:`, resolved
# from that speaker's own (validated) config in _final_validate below, once all components have
# been validated. audio_codec is the one thing that genuinely can't be read off the speaker: it's
# a property of the video FILE's audio track (how it was encoded), not the playback hardware, so
# it stays a real user-facing option here. Every video's audio track must match the resolved
# format (and the chosen codec) exactly; a mismatch is a hard runtime error for that file
# (video-only playback), not something this component resizes itself around.
AUDIO_CODEC_PCM = "pcm"
AUDIO_CODEC_MP3 = "mp3"
AUDIO_CODEC_FLAC = "flac"
AUDIO_CODECS = (AUDIO_CODEC_PCM, AUDIO_CODEC_MP3, AUDIO_CODEC_FLAC)

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


def _validate_audio_codec_required(config):
    if CONF_SPEAKER_ID in config and CONF_AUDIO_CODEC not in config:
        raise cv.Invalid(
            "speaker_id is set: audio_codec must be set too -- it's the one audio format detail "
            "that can't be read from the speaker's own config (sample_rate/channels/"
            "bits_per_sample are; see audio_codec's own comment)"
        )
    return config


# Component configuration
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SimpleVideoPlayer),
            cv.Required(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
            cv.Optional(
                CONF_CACHE_BUFFER_SIZE, default=DEFAULT_CACHE_BUFFER_SIZE
            ): cv.All(cv.validate_bytes, cv.Range(min=MIN_CACHE_BUFFER_SIZE, max=MAX_CACHE_BUFFER_SIZE)),
            cv.Optional(
                CONF_INPUT_BUFFER_SIZE, default=DEFAULT_INPUT_BUFFER_SIZE
            ): cv.All(cv.validate_bytes, cv.Range(min=MIN_INPUT_BUFFER_SIZE, max=MAX_INPUT_BUFFER_SIZE)),
            cv.Optional(
                CONF_PREFETCH_FRAMES, default=DEFAULT_PREFETCH_FRAMES
            ): cv.int_range(min=MIN_PREFETCH_FRAMES, max=MAX_PREFETCH_FRAMES),
            cv.Optional(CONF_TARGET_FPS, default=DEFAULT_TARGET_FPS): cv.float_range(
                min=MIN_FPS, max=MAX_FPS
            ),
            # The codec every video's audio track is encoded with (required together with
            # speaker_id -- see AUDIO_CODEC_PCM comment above). sample_rate/channels/
            # bits_per_sample are deliberately NOT options here: they're resolved from the
            # referenced speaker's own config in _final_validate, not asked twice.
            cv.Optional(CONF_AUDIO_CODEC): cv.one_of(*AUDIO_CODECS, lower=True),
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
    _validate_audio_codec_required,
)


def _resolve_speaker_audio_format(config, fconf):
    # sample_rate/bits_per_sample/channel count are properties of the SPEAKER hardware
    # (config[CONF_SPEAKER_ID] points at it), not something to ask the user to repeat here --
    # read them from that speaker's own already-validated config instead. This has to run in
    # FINAL_VALIDATE_SCHEMA, not CONFIG_SCHEMA/to_code: final-validation is the one pass
    # guaranteed to run after every component's own CONFIG_SCHEMA (the speaker's included) has
    # already resolved defaults/derived fields, regardless of YAML ordering.
    speaker_id = config[CONF_SPEAKER_ID]
    try:
        speaker_path = fconf.get_path_for_id(speaker_id)[:-1]
        speaker_conf = fconf.get_config_for_path(speaker_path)
    except KeyError as err:
        raise cv.Invalid(f"Could not resolve speaker_id '{speaker_id}' to its own config") from err

    if CONF_SAMPLE_RATE not in speaker_conf or CONF_BITS_PER_SAMPLE not in speaker_conf:
        raise cv.Invalid(
            f"speaker '{speaker_id}' does not declare both sample_rate and bits_per_sample -- "
            "simple_video_player needs a speaker platform that fixes both explicitly."
        )
    sample_rate = speaker_conf[CONF_SAMPLE_RATE]
    bits_per_sample = speaker_conf[CONF_BITS_PER_SAMPLE]

    if CONF_NUM_CHANNELS in speaker_conf:
        num_channels = speaker_conf[CONF_NUM_CHANNELS]
    elif CONF_CHANNEL in speaker_conf and speaker_conf[CONF_CHANNEL] in SPEAKER_CHANNEL_MODES:
        # mono/left/right all mean "1 channel out"; only stereo is 2 -- same mapping
        # SPEAKER_CHANNEL_MODES itself encodes on the C++ side.
        num_channels = 2 if speaker_conf[CONF_CHANNEL] == "stereo" else 1
    else:
        raise cv.Invalid(
            f"speaker '{speaker_id}' does not declare a channel count (no num_channels or "
            "channel) -- simple_video_player needs a speaker platform that fixes this explicitly."
        )

    # Stash the resolved values on THIS component's own validated config, the same
    # get_path_for_id()/get_config_for_path() pattern (see mpr121/__init__.py) -- not the `config`
    # parameter directly, since final_validate must not assume that's the live object backing
    # full_config. to_code() reads these back out below.
    this_path = fconf.get_path_for_id(config[CONF_ID])[:-1]
    this_conf = fconf.get_config_for_path(this_path)
    this_conf[CONF_AUDIO_SAMPLE_RATE] = sample_rate
    this_conf[CONF_AUDIO_BITS_PER_SAMPLE] = bits_per_sample
    this_conf[CONF_AUDIO_CHANNELS] = num_channels
    if CONF_CHANNEL in speaker_conf:
        this_conf[CONF_RESOLVED_SPEAKER_CHANNEL] = speaker_conf[CONF_CHANNEL]


def _final_validate(config):
    if CONF_SPEAKER_ID not in config:
        return config

    fconf = fv.full_config.get()
    _resolve_speaker_audio_format(config, fconf)

    # Codec support (FLAC/MP3) is enabled by the user's own `audio: codecs:` block, never by
    # simple_video_player itself. audio_codec now fixes which ONE codec every video's audio track
    # must use, so this is a hard requirement for that one codec, not a "some videos might use
    # this" warning -- catch it here at compile time instead of failing at runtime on the device.
    codec = config.get(CONF_AUDIO_CODEC)
    if codec not in (AUDIO_CODEC_MP3, AUDIO_CODEC_FLAC):
        return config  # PCM needs no decoder / no `audio: codecs:` entry at all

    audio_config = fconf.get("audio")
    if isinstance(audio_config, list):
        # Defensive: audio is documented as single-instance, but don't assume forever.
        audio_config = audio_config[0] if audio_config else None

    codecs_config = None
    if isinstance(audio_config, dict):
        codecs_config = audio_config.get(CONF_CODECS)

    key = CONF_MP3 if codec == AUDIO_CODEC_MP3 else CONF_FLAC
    if not isinstance(codecs_config, dict) or key not in codecs_config:
        raise cv.Invalid(
            f"audio_codec: {codec} is configured, but `audio: codecs: {codec}:` is not enabled -- "
            f"add it, or change audio_codec to match what's actually enabled under `audio:`."
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

        # Fixed audio format: sample_rate/channels/bits_per_sample were resolved from the
        # speaker's own config by _final_validate above (not asked of the user twice) and
        # stashed onto this config; audio_codec is the one the user actually set. Passed down as
        # defines so the C++ side can size its permanent audio buffers as compile-time constants
        # in setup(), once, instead of recomputing them from a file's audio header every play().
        cg.add_define("SVP_AUDIO_SAMPLE_RATE", config[CONF_AUDIO_SAMPLE_RATE])
        cg.add_define("SVP_AUDIO_SOURCE_CHANNELS", config[CONF_AUDIO_CHANNELS])
        cg.add_define("SVP_AUDIO_BITS_PER_SAMPLE", config[CONF_AUDIO_BITS_PER_SAMPLE])
        cg.add_define(f"SVP_AUDIO_CODEC_{config[CONF_AUDIO_CODEC].upper()}")

        # Speaker's own channel mode (mono/left/right/stereo), resolved the same way -- used for
        # stereo<->mono downmix routing (see convert_audio_channels_() in the C++ side).
        channel_mode = config.get(CONF_RESOLVED_SPEAKER_CHANNEL)
        if channel_mode in SPEAKER_CHANNEL_MODES:
            cg.add(var.set_speaker_channel_mode(SPEAKER_CHANNEL_MODES[channel_mode]))

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
