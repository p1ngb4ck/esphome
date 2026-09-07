from __future__ import annotations

from esphome import automation
import esphome.codegen as cg
from esphome.components import speaker
from esphome.components.audio import CONF_CODECS, CONF_FLAC, CONF_MP3
from esphome.components.esp32 import only_on_variant
from esphome.components.esp32.const import VARIANT_ESP32P4
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
from esphome.core import CORE, CoroPriority, coroutine_with_priority
import esphome.final_validate as fv

# Import LVGL canvas type for proper widget ID validation
try:
    from esphome.components.lvgl.widgets.canvas import lv_canvas_t

    LVGL_AVAILABLE = True
except ImportError:
    LVGL_AVAILABLE = False
    lv_canvas_t = None

# Optional: render straight into the MIPI-DSI panel framebuffers (HW JPEG decode -> PPA rotate ->
# DSI flip), bypassing LVGL for the video path entirely. Requires the mipi_dsi display component.
try:
    from esphome.components.mipi_dsi.display import MipiDsi
except ImportError:
    MipiDsi = None

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage"]
AUTO_LOAD = ["image", "audio"]

# Namespaces
simple_video_player_ns = cg.esphome_ns.namespace("simple_video_player")

# Classes
SimpleVideoPlayer = simple_video_player_ns.class_("SimpleVideoPlayer", cg.Component)

# Enums for speaker channel configuration -- is_class=True: the C++ side is a scoped `enum class`,
# so codegen must emit `SpeakerChannelMode::SPEAKER_CHANNEL_*`, not a bare `SPEAKER_CHANNEL_*`.
SpeakerChannelMode = simple_video_player_ns.enum("SpeakerChannelMode", is_class=True)
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
CONF_DISPLAY_ID = "display_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_CACHE_BUFFER_SIZE = "cache_buffer_size"
CONF_INPUT_BUFFER_SIZE = "input_buffer_size"
CONF_PREFETCH_DURATION = "prefetch_duration"
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
# How much of the video's COMPRESSED source stream to keep prefetched (loader task, Core 0 ->
# decode task, Core 1) at all times, expressed as TIME, not a fixed slot count -- the number of
# ring slots this actually needs depends on target_fps (resolved in setup(), once both this and
# target_fps are known: slots = ceil(prefetch_duration * target_fps)). 1s is a sane default that
# comfortably absorbs real storage-read jitter without over-committing PSRAM.
DEFAULT_PREFETCH_DURATION = "1s"
DEFAULT_TARGET_FPS = 30.0

# Validation ranges
MIN_CACHE_BUFFER_SIZE = 8 * 1024  # 8KB
MAX_CACHE_BUFFER_SIZE = 128 * 1024  # 128KB - increased for performance
MIN_INPUT_BUFFER_SIZE = 128 * 1024  # 128KB
MAX_INPUT_BUFFER_SIZE = 2 * 1024 * 1024  # 2MB
MIN_PREFETCH_DURATION_MS = 100  # below this, storage-read jitter has essentially no headroom
MAX_PREFETCH_DURATION_MS = 5000  # above this, PSRAM cost stops being worth the extra headroom
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


def _validate_output_target(config):
    # Exactly one output: an LVGL canvas widget (canvas_id) OR a mipi_dsi panel we render straight
    # into (display_id). They are mutually exclusive -- display_id bypasses LVGL for the video path.
    has_canvas = CONF_CANVAS_ID in config
    has_display = CONF_DISPLAY_ID in config
    if has_canvas and has_display:
        raise cv.Invalid(
            f"Use either '{CONF_CANVAS_ID}' (render into an LVGL canvas) or '{CONF_DISPLAY_ID}' "
            "(render straight into a mipi_dsi panel), not both."
        )
    if not has_canvas and not has_display:
        raise cv.Invalid(
            f"An output is required: set '{CONF_CANVAS_ID}' to render into an LVGL canvas, or "
            f"'{CONF_DISPLAY_ID}' to render straight into a mipi_dsi panel."
        )
    return config


# Component configuration
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SimpleVideoPlayer),
            # Output target -- exactly one of these (enforced by _validate_output_target):
            #   canvas_id  : render into an LVGL canvas widget (the original path)
            #   display_id : render straight into this mipi_dsi panel's framebuffers (HW JPEG
            #                decode -> PPA rotate -> DSI VSYNC flip), bypassing LVGL for the video
            #                path. Rotation is read from the LVGL component at runtime.
            cv.Optional(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
            **(
                {cv.Optional(CONF_DISPLAY_ID): cv.use_id(MipiDsi)}
                if MipiDsi is not None
                else {}
            ),
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
            cv.Optional(
                CONF_CACHE_BUFFER_SIZE, default=DEFAULT_CACHE_BUFFER_SIZE
            ): cv.All(cv.validate_bytes, cv.Range(min=MIN_CACHE_BUFFER_SIZE, max=MAX_CACHE_BUFFER_SIZE)),
            cv.Optional(
                CONF_INPUT_BUFFER_SIZE, default=DEFAULT_INPUT_BUFFER_SIZE
            ): cv.All(cv.validate_bytes, cv.Range(min=MIN_INPUT_BUFFER_SIZE, max=MAX_INPUT_BUFFER_SIZE)),
            cv.Optional(
                CONF_PREFETCH_DURATION, default=DEFAULT_PREFETCH_DURATION
            ): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=MIN_PREFETCH_DURATION_MS),
                         max=cv.TimePeriod(milliseconds=MAX_PREFETCH_DURATION_MS)),
            ),
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
    _validate_output_target,
    only_on_variant(supported=[VARIANT_ESP32P4], msg_prefix="simple_video_player"),
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

    # The speaker's channel mode is part of the fixed-format lock. Prefer the speaker's own
    # explicit `channel:` key (mono/left/right/stereo); when the speaker platform only declares
    # `num_channels:` (no `channel:` key), derive it from the resolved count so to_code() ALWAYS
    # emits set_speaker_channel_mode() and the C++ side never silently keeps its mono default
    # while SVP_AUDIO_SOURCE_CHANNELS says 2. No routing or downmix is implied -- source channel
    # count equals the speaker's by construction (the user transcodes every file to match); this
    # only selects the one fixed count the speaker stream-info is built with.
    if CONF_CHANNEL in speaker_conf and speaker_conf[CONF_CHANNEL] in SPEAKER_CHANNEL_MODES:
        resolved_channel = speaker_conf[CONF_CHANNEL]
    else:
        resolved_channel = "stereo" if num_channels == 2 else "mono"

    # Stash the resolved values on THIS component's own validated config, the same
    # get_path_for_id()/get_config_for_path() pattern (see mpr121/__init__.py) -- not the `config`
    # parameter directly, since final_validate must not assume that's the live object backing
    # full_config. to_code() reads these back out below.
    this_path = fconf.get_path_for_id(config[CONF_ID])[:-1]
    this_conf = fconf.get_config_for_path(this_path)
    this_conf[CONF_AUDIO_SAMPLE_RATE] = sample_rate
    this_conf[CONF_AUDIO_BITS_PER_SAMPLE] = bits_per_sample
    this_conf[CONF_AUDIO_CHANNELS] = num_channels
    this_conf[CONF_RESOLVED_SPEAKER_CHANNEL] = resolved_channel


def _validate_display_output(config, fconf):
    """Rendering straight into the mipi_dsi framebuffers needs >= 2 (one scanning out, one being
    written) so the DPI driver can flip at VSYNC. Rotation (90/180/270) additionally needs a 3rd
    FB for the non-blocking PPA pipeline -- that's checked at runtime in init_dsi_output_() since
    the rotation only comes from the LVGL component."""
    display_id = config[CONF_DISPLAY_ID]
    try:
        display_path = fconf.get_path_for_id(display_id)[:-1]
        display_conf = fconf.get_config_for_path(display_path)
    except KeyError as err:
        raise cv.Invalid(
            f"Could not resolve display_id '{display_id}' to its own config"
        ) from err
    fbs = display_conf.get("frame_buffers", 1)
    if fbs < 2:
        raise cv.Invalid(
            f"display_id '{display_id}' has frame_buffers: {fbs}; simple_video_player direct "
            "output needs frame_buffers: 2 (3 if the video is rotated on-device). Each buffer is "
            "width*height*bpp in PSRAM."
        )


def _final_validate(config):
    fconf = fv.full_config.get()

    if CONF_DISPLAY_ID in config:
        _validate_display_output(config, fconf)

    if CONF_SPEAKER_ID not in config:
        return config

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


@coroutine_with_priority(CoroPriority.FINAL)
async def _resolve_display_output(var, display_id):
    """Runs after every component's to_code(), when the mipi_dsi display variable is guaranteed to
    exist. Wire it into the player. Rotation is read from the LVGL component at runtime (setup()),
    not plumbed through here -- ESPHome forbids `rotation:` on an LVGL-driven display, so LVGL's is
    the authoritative rotation."""
    disp = await cg.get_variable(display_id)
    cg.add(var.set_dsi(disp))
    cg.add_define("SVP_DSI_OUTPUT")


async def to_code(config):
    # Defines USE_HWJPG (P4). Required: the LVGL canvas dma_buffer path allocates its draw buffer
    # via jpeg_alloc_decoder_mem() only when USE_HWJPG is defined, otherwise falls back to plain
    # lv_malloc_core() -- which the P4 hardware JPEG decoder rejects as unaligned.
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

    # Output target: LVGL canvas OR direct mipi_dsi (mutually exclusive, see
    # _validate_output_target). The mipi_dsi wiring is deferred to a FINAL codegen coroutine so the
    # display variable is guaranteed to exist by then.
    if CONF_CANVAS_ID in config:
        canvas = await cg.get_variable(config[CONF_CANVAS_ID])
        cg.add(var.set_canvas(canvas))
    if CONF_DISPLAY_ID in config:
        CORE.add_job(_resolve_display_output, var, config[CONF_DISPLAY_ID])

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

        # Speaker's channel mode -- always resolved by _final_validate (from the speaker's own
        # `channel:` key, or derived from its `num_channels:` when it has none), so this is always
        # emitted and the C++ side never falls back to its mono default. Part of the fixed-format
        # lock: source channel count equals the speaker's, no runtime conversion.
        channel_mode = config[CONF_RESOLVED_SPEAKER_CHANNEL]
        cg.add(var.set_speaker_channel_mode(SPEAKER_CHANNEL_MODES[channel_mode]))

    # Set buffer sizes
    cg.add(var.set_cache_buffer_size(config[CONF_CACHE_BUFFER_SIZE]))
    cg.add(var.set_input_buffer_size(config[CONF_INPUT_BUFFER_SIZE]))
    cg.add(var.set_prefetch_duration_ms(config[CONF_PREFETCH_DURATION].total_milliseconds))

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
