from __future__ import annotations

from esphome import automation
import esphome.codegen as cg
from esphome.components import display, media_player, speaker, touchscreen
from esphome.components.audio import CONF_CODECS, CONF_FLAC, CONF_MP3
from esphome.components.esp32 import only_on_variant, request_http_client
from esphome.components.esp32.const import VARIANT_ESP32P4
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_CHANNEL,
    CONF_HEIGHT,
    CONF_ID,
    CONF_MODE,
    CONF_NUM_CHANNELS,
    CONF_PLATFORM,
    CONF_POSITION,
    CONF_SAMPLE_RATE,
    CONF_TRIGGER_ID,
    CONF_URL,
    CONF_VOLUME,
    CONF_WIDTH,
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

try:
    from esphome.components.lvgl.widgets.dropdown import lv_dropdown_t
except ImportError:
    lv_dropdown_t = None

try:
    from esphome.components.lvgl import defines as lvgl_defines
    from esphome.components.lvgl.types import lv_obj_base_t, lv_style_t
except ImportError:
    lvgl_defines = None
    lv_obj_base_t = None
    lv_style_t = None

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage"]

CONF_PICTURE_CANVAS_ID = "picture_canvas_id"


def AUTO_LOAD(config):
    load = ["image", "audio", "json"]
    # Pictures are decoded by runtime_image (its formats enabled in to_code).
    if config and CONF_PICTURE_CANVAS_ID in config:
        load.append("runtime_image")
    return load


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
ChannelRefreshAction = simple_video_player_ns.class_("ChannelRefreshAction", automation.Action)
ChannelPlayAction = simple_video_player_ns.class_("ChannelPlayAction", automation.Action)
ChannelStopAction = simple_video_player_ns.class_("ChannelStopAction", automation.Action)
StreamSleepAction = simple_video_player_ns.class_("StreamSleepAction", automation.Action)
StreamWakeAction = simple_video_player_ns.class_("StreamWakeAction", automation.Action)
StreamOutputAction = simple_video_player_ns.class_("StreamOutputAction", automation.Action)
SetVolumeAction = simple_video_player_ns.class_("SetVolumeAction", automation.Action)
SeekAction = simple_video_player_ns.class_("SeekAction", automation.Action)
NextAction = simple_video_player_ns.class_("NextAction", automation.Action)
TogglePauseAction = simple_video_player_ns.class_("TogglePauseAction", automation.Action)
PreviousAction = simple_video_player_ns.class_("PreviousAction", automation.Action)
RepeatAction = simple_video_player_ns.class_("RepeatAction", automation.Action)
VolumeStepAction = simple_video_player_ns.class_("VolumeStepAction", automation.Action)
ShowImageAction = simple_video_player_ns.class_("ShowImageAction", automation.Action)
PictureDirectoryAction = simple_video_player_ns.class_("PictureDirectoryAction", automation.Action)
PictureCommandAction = simple_video_player_ns.class_("PictureCommandAction", automation.Action)
FullscreenAction = simple_video_player_ns.class_("FullscreenAction", automation.Action)
PictureCommand = simple_video_player_ns.enum("PictureCommand", is_class=True)
ThumbnailClickTrigger = simple_video_player_ns.class_(
    "ThumbnailClickTrigger", automation.Trigger.template(cg.size_t)
)
ImageFitMode = simple_video_player_ns.enum("ImageFitMode", is_class=True)
IMAGE_FIT_MODES = {
    "SCALE_TO_FIT": ImageFitMode.SCALE_TO_FIT,
    "SCALE_TO_FILL": ImageFitMode.SCALE_TO_FILL,
    "STRETCH": ImageFitMode.STRETCH,
    "CENTER": ImageFitMode.CENTER,
}
ThumbnailLayout = simple_video_player_ns.enum("ThumbnailLayout", is_class=True)
THUMBNAIL_LAYOUTS = {
    "HORIZONTAL": ThumbnailLayout.HORIZONTAL,
    "VERTICAL": ThumbnailLayout.VERTICAL,
    "GRID": ThumbnailLayout.GRID,
}
ThumbnailSlideEdge = simple_video_player_ns.enum("ThumbnailSlideEdge", is_class=True)
THUMBNAIL_SLIDE_EDGES = {
    "LEFT": ThumbnailSlideEdge.LEFT,
    "RIGHT": ThumbnailSlideEdge.RIGHT,
    "TOP": ThumbnailSlideEdge.TOP,
    "BOTTOM": ThumbnailSlideEdge.BOTTOM,
}
RepeatMode = simple_video_player_ns.enum("RepeatMode", is_class=True)
REPEAT_MODES = {
    "off": RepeatMode.OFF,
    "file": RepeatMode.FILE,
    "playlist": RepeatMode.PLAYLIST,
}
REPEAT_CYCLE = "cycle"

# Configuration keys
CONF_CANVAS_ID = "canvas_id"
CONF_DISPLAY_ID = "display_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_MEDIA_PLAYER_ID = "media_player_id"
CONF_CACHE_BUFFER_SIZE = "cache_buffer_size"
CONF_INPUT_BUFFER_SIZE = "input_buffer_size"
CONF_PREFETCH_DURATION = "prefetch_duration"
CONF_TARGET_FPS = "target_fps"
CONF_STREAM_PORT = "stream_port"
CONF_STREAM_MAX_FRAME_BYTES = "stream_max_frame_bytes"
CONF_STREAM_OUTPUT = "stream_output"
STREAM_OUTPUT_FULLSCREEN = "fullscreen"
STREAM_OUTPUT_CANVAS = "canvas"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_CHANNEL_LIST = "channel_list"
CONF_TOKEN = "token"
CONF_WIDGET_ID = "widget_id"
CONF_AUDIO_CODEC = "audio_codec"
CONF_AUDIO_LATENCY = "audio_latency"
CONF_RELATIVE = "relative"
CONF_PLAYLIST = "playlist"
CONF_CONTROLS = "controls"
CONF_SEEK_BAR_ID = "seek_bar_id"
CONF_POSITION_LABEL_ID = "position_label_id"
CONF_TITLE_LABEL_ID = "title_label_id"
CONF_PLAY_PAUSE_ID = "play_pause_id"
CONF_REPEAT_ID = "repeat_id"
CONF_STEP = "step"
CONF_HIDE_AFTER = "hide_after"
CONF_READ_AHEAD_BUFFER_SIZE = "read_ahead_buffer_size"
CONF_PICTURE_DIRECTORIES = "picture_directories"
CONF_SLIDESHOW_INTERVAL = "slideshow_interval"
CONF_FIT_MODE = "fit_mode"
CONF_OVERLAY_ICON_SIZE = "overlay_icon_size"
CONF_OVERLAY_ICON_COLOR = "overlay_icon_color"
CONF_OVERLAY_DURATION = "overlay_duration"
CONF_DEFAULT_IMAGE_INDEX = "default_image_index"
CONF_LONG_PRESS_TIME = "long_press_time"
CONF_ENABLE_THUMBNAILS = "enable_thumbnails"
CONF_THUMBNAIL_WIDTH = "thumbnail_width"
CONF_THUMBNAIL_HEIGHT = "thumbnail_height"
CONF_THUMBNAIL_MAX_COUNT = "thumbnail_max_count"
CONF_THUMBNAIL_LAZY_LOAD = "thumbnail_lazy_load"
CONF_THUMBNAIL_PRELOAD_COUNT = "thumbnail_preload_count"
CONF_THUMBNAIL_LAYOUT = "thumbnail_layout"
CONF_THUMBNAIL_SPACING = "thumbnail_spacing"
CONF_THUMBNAIL_GRID_COLUMNS = "thumbnail_grid_columns"
CONF_THUMBNAIL_CONTAINER_ID = "thumbnail_container_id"
CONF_THUMBNAIL_STYLE_ID = "thumbnail_style_id"
CONF_THUMBNAIL_ACTIVE_STYLE_ID = "thumbnail_active_style_id"
CONF_THUMBNAIL_CONTAINER_STYLE_ID = "thumbnail_container_style_id"
CONF_THUMBNAIL_LABEL_PATTERN = "thumbnail_label_pattern"
CONF_THUMBNAIL_LABEL_STYLE_ID = "thumbnail_label_style_id"
CONF_THUMBNAIL_SLIDE_EDGE = "thumbnail_slide_edge"
CONF_THUMBNAIL_SLIDE_ENABLED = "thumbnail_slide_enabled"
CONF_THUMBNAIL_AUTO_HIDE_TIMEOUT = "thumbnail_auto_hide_timeout"
CONF_ON_THUMBNAIL_CLICK = "on_thumbnail_click"
CONF_INDEX = "index"
CONF_PATH = "path"
CONF_FULLSCREEN = "fullscreen"
PICTURE_FORMATS = ("JPEG", "PNG", "BMP")
# Picture keys with no default: setting one without picture_canvas_id is an error.
PICTURE_ONLY_KEYS = (
    CONF_PICTURE_DIRECTORIES,
    CONF_DEFAULT_IMAGE_INDEX,
    CONF_THUMBNAIL_CONTAINER_ID,
    CONF_THUMBNAIL_STYLE_ID,
    CONF_THUMBNAIL_ACTIVE_STYLE_ID,
    CONF_THUMBNAIL_CONTAINER_STYLE_ID,
    CONF_THUMBNAIL_LABEL_PATTERN,
    CONF_THUMBNAIL_LABEL_STYLE_ID,
    CONF_ON_THUMBNAIL_CLICK,
)
# The read-ahead ring holds the picture the sound runs ahead of: 100 ms per MiB (4MB -> 400 ms).
AUDIO_LATENCY_MS_PER_MIB = 100
DEFAULT_READ_AHEAD_BUFFER_SIZE = 4 * 1024 * 1024
MIN_READ_AHEAD_BUFFER_SIZE = 1000000  # 1MB
MAX_READ_AHEAD_BUFFER_SIZE = 16 * 1024 * 1024


def _next_power_of_two(value):
    # The ring indexes with a mask: "4MB" (4000000) becomes 4 MiB.
    return 1 << (value - 1).bit_length()


def _max_audio_latency_ms(config):
    return config[CONF_READ_AHEAD_BUFFER_SIZE] // (1024 * 1024) * AUDIO_LATENCY_MS_PER_MIB


def _latency_within_max(config):
    latency = config.get(CONF_AUDIO_LATENCY)
    limit = _max_audio_latency_ms(config)
    if isinstance(latency, cv.TimePeriod) and latency.total_milliseconds > limit:
        raise cv.Invalid(
            f"{CONF_AUDIO_LATENCY} can be at most {limit}ms with this "
            f"{CONF_READ_AHEAD_BUFFER_SIZE} ({AUDIO_LATENCY_MS_PER_MIB}ms per MB)",
            [CONF_AUDIO_LATENCY],
        )
    return config
# Internal-only keys (never part of CONFIG_SCHEMA): _final_validate resolves these from the
# referenced speaker's own config and stashes them here for to_code() to read back.
CONF_AUDIO_SAMPLE_RATE = "audio_sample_rate"
CONF_AUDIO_CHANNELS = "audio_channels"
CONF_AUDIO_BITS_PER_SAMPLE = "audio_bits_per_sample"
CONF_RESOLVED_SPEAKER_CHANNEL = "resolved_speaker_channel"
CONF_AUDIO_ROUTER = "audio_router"
# The canvas widget's own width/height: the size of the output buffers and of the picture a
# stream sender is asked for (the device never scales).
CONF_RESOLVED_CANVAS_SIZE = "resolved_canvas_size"

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
MIN_PREFETCH_DURATION_MS = (
    100  # below this, storage-read jitter has essentially no headroom
)
MAX_PREFETCH_DURATION_MS = (
    5000  # above this, PSRAM cost stops being worth the extra headroom
)
MIN_FPS = 1.0
MAX_FPS = 60.0


def _validate_audio_codec_required(config):
    if CONF_SPEAKER_ID in config and not config.get(CONF_AUDIO_CODEC):
        raise cv.Invalid(
            "speaker_id is set: audio_codec must list at least one supported codec -- it's the one "
            "audio format detail that can't be read from the speaker's own config (sample_rate/"
            "channels/bits_per_sample are; see audio_codec's own comment)"
        )
    return config


def _validate_output_target(config):
    # An LVGL canvas widget (canvas_id), a mipi_dsi panel drawn straight into (display_id), or both:
    # each video plays on the one its size fits.
    has_canvas = CONF_CANVAS_ID in config
    has_display = CONF_DISPLAY_ID in config
    if not has_canvas and not has_display:
        raise cv.Invalid(
            f"An output is required: set '{CONF_CANVAS_ID}' to render into an LVGL canvas, "
            f"'{CONF_DISPLAY_ID}' to render straight into a mipi_dsi panel, or both."
        )
    if CONF_STREAM_OUTPUT not in config:
        if CONF_STREAM_PORT in config:
            config[CONF_STREAM_OUTPUT] = STREAM_OUTPUT_FULLSCREEN if has_display else STREAM_OUTPUT_CANVAS
    elif (config[CONF_STREAM_OUTPUT] == STREAM_OUTPUT_FULLSCREEN and not has_display) or (
        config[CONF_STREAM_OUTPUT] == STREAM_OUTPUT_CANVAS and not has_canvas
    ):
        needed = CONF_DISPLAY_ID if config[CONF_STREAM_OUTPUT] == STREAM_OUTPUT_FULLSCREEN else CONF_CANVAS_ID
        raise cv.Invalid(f"{CONF_STREAM_OUTPUT}: {config[CONF_STREAM_OUTPUT]} needs '{needed}'.")
    return config


def _validate_stream(config):
    if CONF_CHANNEL_LIST in config and CONF_STREAM_PORT not in config:
        raise cv.Invalid(f"'{CONF_CHANNEL_LIST}' plays channels through the stream: it needs '{CONF_STREAM_PORT}'.")
    if CONF_TOUCHSCREEN_ID in config and CONF_STREAM_PORT not in config and CONF_CONTROLS not in config:
        raise cv.Invalid(
            f"'{CONF_TOUCHSCREEN_ID}' is for a stream sender ('{CONF_STREAM_PORT}') or '{CONF_CONTROLS}'."
        )
    return config


def _validate_controls(config):
    if CONF_CONTROLS not in config:
        return config
    if CONF_DISPLAY_ID not in config or CONF_TOUCHSCREEN_ID not in config:
        raise cv.Invalid(
            f"'{CONF_CONTROLS}' are drawn over direct display output: they need '{CONF_DISPLAY_ID}' "
            f"and '{CONF_TOUCHSCREEN_ID}'."
        )
    # The controls are rendered off screen with lv_snapshot while LVGL is paused.
    lvgl_defines.add_define("LV_USE_SNAPSHOT", "1")
    return config


def _validate_pictures(config):
    if CONF_PICTURE_CANVAS_ID in config:
        return config
    for key in PICTURE_ONLY_KEYS:
        if key in config:
            raise cv.Invalid(f"'{key}' is for pictures: it needs '{CONF_PICTURE_CANVAS_ID}'.", [key])
    return config


def _picture_schema():
    if lv_canvas_t is None or lv_obj_base_t is None or lv_style_t is None:
        return {}
    return {
        # Pictures (JPEG/PNG/BMP) of a folder in an LVGL canvas (opaque, RGB565): slideshow,
        # play/pause overlay, swipes (left/right: picture, up/down: folder, long press: slideshow)
        # and an optional thumbnail strip in thumbnail_container_id.
        cv.Optional(CONF_PICTURE_CANVAS_ID): cv.use_id(lv_canvas_t),
        cv.Optional(CONF_PICTURE_DIRECTORIES): cv.ensure_list(cv.string),
        cv.Optional(CONF_SLIDESHOW_INTERVAL, default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_FIT_MODE, default="SCALE_TO_FIT"): cv.enum(IMAGE_FIT_MODES, upper=True),
        cv.Optional(CONF_OVERLAY_ICON_SIZE, default=128): cv.int_range(min=32, max=256),
        cv.Optional(CONF_OVERLAY_ICON_COLOR, default=0xFFFFFF): cv.hex_uint32_t,
        cv.Optional(CONF_OVERLAY_DURATION, default="1500ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(milliseconds=500), max=cv.TimePeriod(milliseconds=5000)),
        ),
        # Shown once the folder is listed; off: nothing until asked for.
        cv.Optional(CONF_DEFAULT_IMAGE_INDEX): cv.Any(cv.int_range(min=0), cv.one_of("off", lower=True)),
        cv.Optional(CONF_LONG_PRESS_TIME, default="1000ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ENABLE_THUMBNAILS, default=True): cv.boolean,
        cv.Optional(CONF_THUMBNAIL_WIDTH, default=120): cv.int_range(min=32, max=320),
        cv.Optional(CONF_THUMBNAIL_HEIGHT, default=90): cv.int_range(min=32, max=240),
        # Slots of the strip (PSRAM, width x height x 2 bytes each); it scrolls through the folder.
        cv.Optional(CONF_THUMBNAIL_MAX_COUNT, default=10): cv.int_range(min=1, max=50),
        cv.Optional(CONF_THUMBNAIL_LAZY_LOAD, default=True): cv.boolean,
        cv.Optional(CONF_THUMBNAIL_PRELOAD_COUNT, default=5): cv.int_range(min=0, max=50),
        cv.Optional(CONF_THUMBNAIL_LAYOUT, default="HORIZONTAL"): cv.enum(THUMBNAIL_LAYOUTS, upper=True),
        cv.Optional(CONF_THUMBNAIL_SPACING, default=5): cv.int_range(min=0, max=50),
        cv.Optional(CONF_THUMBNAIL_GRID_COLUMNS, default=3): cv.int_range(min=1, max=10),
        cv.Optional(CONF_THUMBNAIL_CONTAINER_ID): cv.use_id(lv_obj_base_t),
        cv.Optional(CONF_THUMBNAIL_STYLE_ID): cv.use_id(lv_style_t),
        cv.Optional(CONF_THUMBNAIL_ACTIVE_STYLE_ID): cv.use_id(lv_style_t),
        cv.Optional(CONF_THUMBNAIL_CONTAINER_STYLE_ID): cv.use_id(lv_style_t),
        # {index} (1-based), {filename}, {name} (without extension)
        cv.Optional(CONF_THUMBNAIL_LABEL_PATTERN): cv.string,
        cv.Optional(CONF_THUMBNAIL_LABEL_STYLE_ID): cv.use_id(lv_style_t),
        cv.Optional(CONF_THUMBNAIL_SLIDE_EDGE, default="RIGHT"): cv.enum(THUMBNAIL_SLIDE_EDGES, upper=True),
        cv.Optional(CONF_THUMBNAIL_SLIDE_ENABLED, default=False): cv.boolean,
        cv.Optional(CONF_THUMBNAIL_AUTO_HIDE_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
        # Replaces showing the tapped picture; x is its index.
        cv.Optional(CONF_ON_THUMBNAIL_CLICK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ThumbnailClickTrigger)}
        ),
    }


async def _pictures_to_code(var, config):
    from esphome.components.runtime_image import enable_format

    # JPEG goes through esp32.require_hw_jpeg() (the P4's JPEG unit).
    for name in PICTURE_FORMATS:
        enable_format(name)
    cg.add_define("SVP_PICTURES")
    cg.add(var.set_picture_canvas(await cg.get_variable(config[CONF_PICTURE_CANVAS_ID])))
    for path in config.get(CONF_PICTURE_DIRECTORIES, []):
        cg.add(var.add_picture_directory(path))
    cg.add(var.set_slideshow_interval(config[CONF_SLIDESHOW_INTERVAL]))
    cg.add(var.set_fit_mode(config[CONF_FIT_MODE]))
    cg.add(var.set_overlay_icon_size(config[CONF_OVERLAY_ICON_SIZE]))
    cg.add(var.set_overlay_icon_color(config[CONF_OVERLAY_ICON_COLOR]))
    cg.add(var.set_overlay_duration(config[CONF_OVERLAY_DURATION]))
    if (index := config.get(CONF_DEFAULT_IMAGE_INDEX)) is not None:
        cg.add(var.set_default_image_index(-1 if index == "off" else index))
    cg.add(var.set_long_press_time(config[CONF_LONG_PRESS_TIME]))
    cg.add(var.set_enable_thumbnails(config[CONF_ENABLE_THUMBNAILS]))
    cg.add(var.set_thumbnail_size(config[CONF_THUMBNAIL_WIDTH], config[CONF_THUMBNAIL_HEIGHT]))
    cg.add(var.set_thumbnail_max_count(config[CONF_THUMBNAIL_MAX_COUNT]))
    cg.add(var.set_thumbnail_lazy_load(config[CONF_THUMBNAIL_LAZY_LOAD]))
    cg.add(var.set_thumbnail_preload_count(config[CONF_THUMBNAIL_PRELOAD_COUNT]))
    cg.add(var.set_thumbnail_layout(config[CONF_THUMBNAIL_LAYOUT]))
    cg.add(var.set_thumbnail_spacing(config[CONF_THUMBNAIL_SPACING]))
    cg.add(var.set_thumbnail_grid_columns(config[CONF_THUMBNAIL_GRID_COLUMNS]))
    for key, setter in (
        (CONF_THUMBNAIL_CONTAINER_ID, "set_thumbnail_container"),
        (CONF_THUMBNAIL_STYLE_ID, "set_thumbnail_style"),
        (CONF_THUMBNAIL_ACTIVE_STYLE_ID, "set_thumbnail_active_style"),
        (CONF_THUMBNAIL_CONTAINER_STYLE_ID, "set_thumbnail_container_style"),
        (CONF_THUMBNAIL_LABEL_STYLE_ID, "set_thumbnail_label_style"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(await cg.get_variable(config[key])))
    if CONF_THUMBNAIL_LABEL_PATTERN in config:
        cg.add(var.set_thumbnail_label_pattern(config[CONF_THUMBNAIL_LABEL_PATTERN]))
    cg.add(var.set_thumbnail_slide_edge(config[CONF_THUMBNAIL_SLIDE_EDGE]))
    cg.add(var.set_thumbnail_slide_enabled(config[CONF_THUMBNAIL_SLIDE_ENABLED]))
    cg.add(var.set_thumbnail_auto_hide_timeout(config[CONF_THUMBNAIL_AUTO_HIDE_TIMEOUT]))
    for conf in config.get(CONF_ON_THUMBNAIL_CLICK, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.size_t, "x")], conf)


def _request_fast_network(config):
    # As Portall does whenever it receives over the network: ESPHome's high-performance networking
    # (TCP window, Wi-Fi buffers), and the runtime controls stream_receiver.cpp uses to hold off
    # Wi-Fi roaming scans and power saving while a stream is arriving.
    if CONF_STREAM_PORT not in config:
        return config
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
    return config


# Component configuration
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SimpleVideoPlayer),
            # Outputs -- one or both (_validate_output_target); a video plays on the one it fits:
            #   canvas_id  : render into an LVGL canvas widget, centred when smaller
            #   display_id : draw each decoded frame onto this display with draw_pixels_at() while
            #                LVGL is paused (Portall's way); the video must match the panel exactly.
            cv.Optional(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
            cv.Optional(CONF_DISPLAY_ID): cv.use_id(display.Display),
            # Network receiver: an AVI (svp_relay) plays like a file, into either output. With
            # display_id also Portall-style udisp JPEG rectangles, drawn while no file plays.
            cv.Optional(CONF_STREAM_PORT): cv.All(cv.requires_component("network"), cv.port),
            cv.Optional(CONF_STREAM_MAX_FRAME_BYTES, default=131072): cv.int_range(
                min=16384, max=1048576
            ),
            # The output a stream sender makes its picture for (stream.output changes it at runtime):
            # fullscreen (display_id, the default with it) or canvas.
            cv.Optional(CONF_STREAM_OUTPUT): cv.one_of(
                STREAM_OUTPUT_FULLSCREEN, STREAM_OUTPUT_CANVAS, lower=True
            ),
            # Touches go back to the stream sender, which replays them into its page.
            cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(touchscreen.Touchscreen),
            # Channel list from the svp_relay Home Assistant integration, shown in an LVGL dropdown;
            # channels.play asks the relay to stream the selected channel to stream_port.
            # On-screen controls drawn over the video while it plays on the panel (LVGL paused): an
            # LVGL object laid out where they show (hidden; opaque background), whose widgets get
            # LV_EVENT_CLICKED (on_click) when tapped. Optional parts kept up to date by the player.
            **(
                {
                    cv.Optional(CONF_CONTROLS): cv.All(
                        cv.requires_component("lvgl"),
                        cv.Schema(
                            {
                                cv.Required(CONF_WIDGET_ID): cv.use_id(lv_obj_base_t),
                                cv.Optional(CONF_SEEK_BAR_ID): cv.use_id(lv_obj_base_t),
                                cv.Optional(CONF_POSITION_LABEL_ID): cv.use_id(lv_obj_base_t),
                                cv.Optional(CONF_TITLE_LABEL_ID): cv.use_id(lv_obj_base_t),
                                cv.Optional(CONF_PLAY_PAUSE_ID): cv.use_id(lv_obj_base_t),
                                cv.Optional(CONF_REPEAT_ID): cv.use_id(lv_obj_base_t),
                                cv.Optional(
                                    CONF_HIDE_AFTER, default="5s"
                                ): cv.positive_time_period_milliseconds,
                            }
                        ),
                    )
                }
                if lv_obj_base_t is not None
                else {}
            ),
            **(
                {
                    cv.Optional(CONF_CHANNEL_LIST): cv.Schema(
                        {
                            cv.Required(CONF_URL): cv.url,
                            cv.Optional(CONF_TOKEN): cv.string,
                            cv.Required(CONF_WIDGET_ID): cv.use_id(lv_dropdown_t),
                        }
                    )
                }
                if lv_dropdown_t is not None
                else {}
            ),
            **_picture_schema(),
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
            # The panel's audio player (mark it `internal: true`): svp_relay plays sound through it with
            # the svp_* actions and shows panel and sound as one media player in Home Assistant.
            cv.Optional(CONF_MEDIA_PLAYER_ID): cv.All(
                cv.requires_component("api"), cv.use_id(media_player.MediaPlayer)
            ),
            cv.Optional(
                CONF_CACHE_BUFFER_SIZE, default=DEFAULT_CACHE_BUFFER_SIZE
            ): cv.All(
                cv.validate_bytes,
                cv.Range(min=MIN_CACHE_BUFFER_SIZE, max=MAX_CACHE_BUFFER_SIZE),
            ),
            cv.Optional(
                CONF_INPUT_BUFFER_SIZE, default=DEFAULT_INPUT_BUFFER_SIZE
            ): cv.All(
                cv.validate_bytes,
                cv.Range(min=MIN_INPUT_BUFFER_SIZE, max=MAX_INPUT_BUFFER_SIZE),
            ),
            cv.Optional(
                CONF_PREFETCH_DURATION, default=DEFAULT_PREFETCH_DURATION
            ): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=MIN_PREFETCH_DURATION_MS),
                    max=cv.TimePeriod(milliseconds=MAX_PREFETCH_DURATION_MS),
                ),
            ),
            cv.Optional(CONF_TARGET_FPS, default=DEFAULT_TARGET_FPS): cv.float_range(
                min=MIN_FPS, max=MAX_FPS
            ),
            # The codec every video's audio track is encoded with (required together with
            # speaker_id -- see AUDIO_CODEC_PCM comment above). sample_rate/channels/
            # bits_per_sample are deliberately NOT options here: they're resolved from the
            # referenced speaker's own config in _final_validate, not asked twice.
            # Which audio codecs this player is built to support. A list (a single value is
            # coerced to a one-element list): the AVI parser detects each file's actual codec at
            # open and the player picks the matching path from this set; a file using a codec not
            # listed here plays video-only. sample_rate/channels/bits_per_sample are still the
            # speaker's fixed format (resolved below), not per-codec.
            cv.Optional(CONF_AUDIO_CODEC): cv.ensure_list(cv.one_of(*AUDIO_CODECS, lower=True)),
            # How long the audio output takes to be heard (a Bluetooth sink's buffer, say): the
            # sound starts this much before the picture. A lambda is read at every start.
            cv.Optional(CONF_AUDIO_LATENCY): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
            # PSRAM ring the file or stream is read ahead into; it also holds the picture the sound
            # runs ahead of, so it sets how large audio_latency can be.
            cv.Optional(
                CONF_READ_AHEAD_BUFFER_SIZE, default=DEFAULT_READ_AHEAD_BUFFER_SIZE
            ): cv.All(
                cv.validate_bytes,
                cv.Range(
                    min=MIN_READ_AHEAD_BUFFER_SIZE, max=MAX_READ_AHEAD_BUFFER_SIZE
                ),
                _next_power_of_two,
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
    _validate_audio_codec_required,
    _latency_within_max,
    _validate_output_target,
    _validate_stream,
    _validate_controls,
    _validate_pictures,
    _request_fast_network,
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
        raise cv.Invalid(
            f"Could not resolve speaker_id '{speaker_id}' to its own config"
        ) from err

    this_path = fconf.get_path_for_id(config[CONF_ID])[:-1]
    this_conf = fconf.get_config_for_path(this_path)

    if speaker_conf.get(CONF_PLATFORM) == CONF_AUDIO_ROUTER:
        # The selected output's format, at runtime: the panel reports it and plays what comes in
        # (the router converts channels and width). Buffers are sized for the largest output, at
        # least 48 kHz stereo; sound reaches the router 16-bit (the relay sends no other).
        from esphome.components.audio_router.speaker import output_formats

        formats = output_formats(fconf, speaker_conf)
        this_conf[CONF_AUDIO_SAMPLE_RATE] = max(
            [48000] + [f[CONF_SAMPLE_RATE] for f in formats]
        )
        this_conf[CONF_AUDIO_BITS_PER_SAMPLE] = 16
        this_conf[CONF_AUDIO_CHANNELS] = max(
            [2] + [f[CONF_NUM_CHANNELS] for f in formats]
        )
        this_conf[CONF_RESOLVED_SPEAKER_CHANNEL] = "stereo"
        this_conf[CONF_AUDIO_ROUTER] = True
        return

    if CONF_SAMPLE_RATE not in speaker_conf or CONF_BITS_PER_SAMPLE not in speaker_conf:
        raise cv.Invalid(
            f"speaker '{speaker_id}' does not declare both sample_rate and bits_per_sample -- "
            "simple_video_player needs a speaker platform that fixes both explicitly."
        )
    sample_rate = speaker_conf[CONF_SAMPLE_RATE]
    bits_per_sample = speaker_conf[CONF_BITS_PER_SAMPLE]

    if CONF_NUM_CHANNELS in speaker_conf:
        num_channels = speaker_conf[CONF_NUM_CHANNELS]
    elif (
        CONF_CHANNEL in speaker_conf
        and speaker_conf[CONF_CHANNEL] in SPEAKER_CHANNEL_MODES
    ):
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
    if (
        CONF_CHANNEL in speaker_conf
        and speaker_conf[CONF_CHANNEL] in SPEAKER_CHANNEL_MODES
    ):
        resolved_channel = speaker_conf[CONF_CHANNEL]
    else:
        resolved_channel = "stereo" if num_channels == 2 else "mono"

    # Stash the resolved values on THIS component's own validated config, the same
    # get_path_for_id()/get_config_for_path() pattern (see mpr121/__init__.py) -- not the `config`
    # parameter directly, since final_validate must not assume that's the live object backing
    # full_config. to_code() reads these back out below.
    this_conf[CONF_AUDIO_SAMPLE_RATE] = sample_rate
    this_conf[CONF_AUDIO_BITS_PER_SAMPLE] = bits_per_sample
    this_conf[CONF_AUDIO_CHANNELS] = num_channels
    this_conf[CONF_RESOLVED_SPEAKER_CHANNEL] = resolved_channel


def _display_depth(full_config, display_id):
    """16 or 24: the display's own `color_depth:` (Portall's display_depth). draw_pixels_at() copies
    straight in only when the picture is in the display's depth, so the decoder must output it."""
    wanted = getattr(display_id, "id", display_id)
    for block in full_config.get("display") or []:
        if getattr(block.get(CONF_ID), "id", None) != wanted:
            continue
        depth = str(block.get("color_depth", "16")).lower().removesuffix("bit")
        return 24 if depth == "24" else 16
    return 16


def _resolve_canvas_size(config, fconf):
    """The canvas widget's size in pixels: the output buffers' size, and what a stream sender is asked for."""
    canvas_id = config[CONF_CANVAS_ID]
    try:
        canvas_conf = fconf.get_config_for_path(fconf.get_path_for_id(canvas_id)[:-1])
    except KeyError as err:
        raise cv.Invalid(f"Could not resolve canvas_id '{canvas_id}' to its own config") from err
    width = canvas_conf.get(CONF_WIDTH)
    height = canvas_conf.get(CONF_HEIGHT)
    if not isinstance(width, int) or not isinstance(height, int) or width <= 0 or height <= 0:
        raise cv.Invalid(
            f"canvas '{canvas_id}' needs its width and height in pixels -- the video and a stream "
            "sender's picture are made exactly that size."
        )
    this_conf = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_ID])[:-1])
    this_conf[CONF_RESOLVED_CANVAS_SIZE] = (width, height)


def _final_validate(config):
    fconf = fv.full_config.get()

    # With display_id, LVGL is paused while a sender is connected; a touch would resume it on top
    # of the stream.
    if CONF_STREAM_PORT in config and CONF_DISPLAY_ID in config:
        for block in fconf.get("lvgl") or []:
            if block.get("resume_on_input", True):
                raise cv.Invalid(
                    f"{CONF_STREAM_PORT}: LVGL is paused while a stream sender is connected, and a "
                    "touch would resume it on top of the stream. Add `resume_on_input: false` to the "
                    "lvgl: block."
                )

    if CONF_CANVAS_ID in config:
        _resolve_canvas_size(config, fconf)

    # The svp_* actions are registered at runtime (CustomAPIDevice::register_service), which the API
    # only builds in with custom_services.
    if CONF_MEDIA_PLAYER_ID in config:
        api_conf = fconf.get("api") or {}
        if not api_conf.get("custom_services", False):
            raise cv.Invalid(
                f"{CONF_MEDIA_PLAYER_ID}: the panel offers its audio actions to Home Assistant at "
                "runtime. Add `custom_services: true` to the api: block."
            )

    if CONF_SPEAKER_ID not in config:
        return config

    _resolve_speaker_audio_format(config, fconf)

    # Codec support (FLAC/MP3) is enabled by the user's own `audio: codecs:` block, never by
    # simple_video_player itself. audio_codec is the list of codecs this player must support, so
    # every COMPRESSED codec listed is a hard requirement -- catch a missing `audio: codecs:` entry
    # here at compile time instead of failing at runtime on the device. PCM needs no decoder / no
    # `audio: codecs:` entry at all, so it's skipped.
    compressed = [c for c in config.get(CONF_AUDIO_CODEC, []) if c in (AUDIO_CODEC_MP3, AUDIO_CODEC_FLAC)]
    if not compressed:
        return config

    audio_config = fconf.get("audio")
    if isinstance(audio_config, list):
        # Defensive: audio is documented as single-instance, but don't assume forever.
        audio_config = audio_config[0] if audio_config else None

    codecs_config = None
    if isinstance(audio_config, dict):
        codecs_config = audio_config.get(CONF_CODECS)

    for codec in compressed:
        key = CONF_MP3 if codec == AUDIO_CODEC_MP3 else CONF_FLAC
        if not isinstance(codecs_config, dict) or key not in codecs_config:
            raise cv.Invalid(
                f"audio_codec lists {codec}, but `audio: codecs: {codec}:` is not enabled -- "
                f"add it, or drop {codec} from audio_codec to match what's enabled under `audio:`."
            )

    return config


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.FINAL)
async def _resolve_display_output(var, display_id):
    """Runs after every component's to_code(), when the display variable is guaranteed to exist."""
    disp = await cg.get_variable(display_id)
    cg.add(var.set_dsi(disp))
    cg.add(var.set_dsi_color_depth(_display_depth(CORE.config, display_id)))
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

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Optional for display_id (paused while a video plays), required by canvas_id via its widget id.
    if lvgl_configs := CORE.config.get("lvgl"):
        cg.add(var.set_lvgl(await cg.get_variable(lvgl_configs[0][CONF_ID])))
        cg.add_define("SVP_USE_LVGL")

    # Outputs: LVGL canvas and/or direct mipi_dsi (see _validate_output_target). The mipi_dsi
    # wiring is deferred to a FINAL codegen coroutine so the display variable is guaranteed to exist
    # by then.
    if CONF_CANVAS_ID in config:
        canvas = await cg.get_variable(config[CONF_CANVAS_ID])
        cg.add(var.set_canvas(canvas))
        cg.add(var.set_canvas_size(*config[CONF_RESOLVED_CANVAS_SIZE]))
        cg.add_define("SVP_CANVAS_OUTPUT")
    if CONF_DISPLAY_ID in config:
        CORE.add_job(_resolve_display_output, var, config[CONF_DISPLAY_ID])

    if CONF_STREAM_PORT in config:
        cg.add(var.set_stream_port(config[CONF_STREAM_PORT]))
        cg.add(var.set_stream_max_frame_bytes(config[CONF_STREAM_MAX_FRAME_BYTES]))
        cg.add(var.init_stream_fullscreen(config[CONF_STREAM_OUTPUT] == STREAM_OUTPUT_FULLSCREEN))
        cg.add_define("SVP_STREAM")
        if CONF_TOUCHSCREEN_ID in config:
            ts = await cg.get_variable(config[CONF_TOUCHSCREEN_ID])
            cg.add(var.set_stream_touchscreen(ts))
            cg.add_define("SVP_STREAM_TOUCH")
        if channels := config.get(CONF_CHANNEL_LIST):
            request_http_client()
            cg.add(var.set_channel_url(channels[CONF_URL].rstrip("/")))
            if CONF_TOKEN in channels:
                cg.add(var.set_channel_token(channels[CONF_TOKEN]))
            widget = await cg.get_variable(channels[CONF_WIDGET_ID])
            cg.add(var.set_channel_widget(widget))
            cg.add_define("SVP_CHANNEL_LIST")

    if controls := config.get(CONF_CONTROLS):
        cg.add_define("SVP_CONTROLS")
        widget = await cg.get_variable(controls[CONF_WIDGET_ID])
        ts = await cg.get_variable(config[CONF_TOUCHSCREEN_ID])
        cg.add(var.set_controls(widget, ts))
        for key, setter in (
            (CONF_SEEK_BAR_ID, "set_controls_seek_bar"),
            (CONF_POSITION_LABEL_ID, "set_controls_position_label"),
            (CONF_TITLE_LABEL_ID, "set_controls_title_label"),
            (CONF_PLAY_PAUSE_ID, "set_controls_play_pause"),
            (CONF_REPEAT_ID, "set_controls_repeat"),
        ):
            if key in controls:
                cg.add(getattr(var, setter)(await cg.get_variable(controls[key])))
        cg.add(var.set_controls_hide_after(controls[CONF_HIDE_AFTER]))

    if CONF_PICTURE_CANVAS_ID in config:
        await _pictures_to_code(var, config)

    # Set speaker (optional - for audio playback)
    if CONF_SPEAKER_ID in config:
        spkr = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_speaker(spkr))
        cg.add_define("USE_SPEAKER")
        cg.add_define("USE_AUDIO")
        if CONF_AUDIO_LATENCY in config:
            latency = await cg.templatable(
                config[CONF_AUDIO_LATENCY],
                [],
                cg.uint32,
                to_exp=lambda period: period.total_milliseconds,
            )
            cg.add(var.set_audio_latency(latency))
        if config.get(CONF_AUDIO_ROUTER):
            cg.add_define("SVP_AUDIO_ROUTER")
            cg.add(var.set_audio_router(spkr))

        # Fixed audio format: sample_rate/channels/bits_per_sample were resolved from the
        # speaker's own config by _final_validate above (not asked of the user twice) and
        # stashed onto this config; audio_codec is the one the user actually set. Passed down as
        # defines so the C++ side can size its permanent audio buffers as compile-time constants
        # in setup(), once, instead of recomputing them from a file's audio header every play().
        cg.add_define("SVP_AUDIO_SAMPLE_RATE", config[CONF_AUDIO_SAMPLE_RATE])
        cg.add_define("SVP_AUDIO_SOURCE_CHANNELS", config[CONF_AUDIO_CHANNELS])
        cg.add_define("SVP_AUDIO_BITS_PER_SAMPLE", config[CONF_AUDIO_BITS_PER_SAMPLE])
        # audio_codec is a list of SUPPORTED codecs: emit one SVP_AUDIO_SUPPORT_<CODEC> define per
        # entry. The C++ side compiles in the decoder path for each supported codec and picks the
        # matching one at runtime from the AVI parser's per-file detection.
        for codec in config[CONF_AUDIO_CODEC]:
            cg.add_define(f"SVP_AUDIO_SUPPORT_{codec.upper()}")

        # Speaker's channel mode -- always resolved by _final_validate (from the speaker's own
        # `channel:` key, or derived from its `num_channels:` when it has none), so this is always
        # emitted and the C++ side never falls back to its mono default. Part of the fixed-format
        # lock: source channel count equals the speaker's, no runtime conversion.
        channel_mode = config[CONF_RESOLVED_SPEAKER_CHANNEL]
        cg.add(var.set_speaker_channel_mode(SPEAKER_CHANNEL_MODES[channel_mode]))

    if CONF_MEDIA_PLAYER_ID in config:
        cg.add(var.set_media_player(await cg.get_variable(config[CONF_MEDIA_PLAYER_ID])))
        cg.add_define("SVP_MEDIA_PLAYER")

    # Set buffer sizes
    cg.add(var.set_cache_buffer_size(config[CONF_CACHE_BUFFER_SIZE]))
    cg.add(var.set_input_buffer_size(config[CONF_INPUT_BUFFER_SIZE]))
    cg.add(var.set_read_ahead_buffer_size(config[CONF_READ_AHEAD_BUFFER_SIZE]))
    cg.add_define("SVP_MAX_AUDIO_LATENCY_MS", _max_audio_latency_ms(config))
    cg.add(
        var.set_prefetch_duration_ms(config[CONF_PREFETCH_DURATION].total_milliseconds)
    )

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
            # folder: every video of the file's folder (by name), so next/previous go through it.
            cv.Optional(CONF_PLAYLIST, default="file"): cv.one_of(
                "file", "folder", lower=True
            ),
        }
    ),
    # play() copies the path into video_path_ and returns before the decode task starts, so the
    # action completes synchronously (the StringRef path is consumed before the call returns).
    synchronous=True,
)
async def simple_video_player_play_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    template_ = await cg.templatable(config["path"], args, cg.std_string)
    cg.add(var.set_path(template_))
    cg.add(var.set_folder_playlist(config[CONF_PLAYLIST] == "folder"))
    return var


@automation.register_action(
    "simple_video_player.toggle",
    TogglePauseAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,
)
async def simple_video_player_toggle_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.next",
    NextAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,
)
async def simple_video_player_next_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


# Back to the start of the file once it has played a few seconds, else the previous item.
@automation.register_action(
    "simple_video_player.previous",
    PreviousAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,
)
async def simple_video_player_previous_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.repeat",
    RepeatAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            # cycle: off -> playlist -> file -> off
            cv.Required(CONF_MODE): cv.Any(
                cv.one_of(REPEAT_CYCLE, lower=True), cv.enum(REPEAT_MODES, lower=True)
            ),
        }
    ),
    synchronous=True,
)
async def simple_video_player_repeat_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    if config[CONF_MODE] == REPEAT_CYCLE:
        cg.add(var.set_cycle(True))
    else:
        cg.add(var.set_mode(config[CONF_MODE]))
    return var


@automation.register_action(
    "simple_video_player.volume_up",
    VolumeStepAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Optional(CONF_STEP, default="5%"): cv.percentage,
        }
    ),
    synchronous=True,
)
async def simple_video_player_volume_up_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_step(config[CONF_STEP]))
    return var


@automation.register_action(
    "simple_video_player.volume_down",
    VolumeStepAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Optional(CONF_STEP, default="5%"): cv.percentage,
        }
    ),
    synchronous=True,
)
async def simple_video_player_volume_down_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_step(-config[CONF_STEP]))
    return var


@automation.register_action(
    "simple_video_player.pause",
    PauseAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,  # just sets the atomic state and returns
)
async def simple_video_player_pause_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.resume",
    ResumeAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,  # just sets the atomic state and returns
)
async def simple_video_player_resume_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


# Jumps in a local AVI file: position in seconds, from the start or (relative) from where it is.
@automation.register_action(
    "simple_video_player.seek",
    SeekAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Required(CONF_POSITION): cv.templatable(cv.float_),
            cv.Optional(CONF_RELATIVE, default=False): cv.boolean,
        }
    ),
    synchronous=True,  # hands the target to the playback task and returns
)
async def simple_video_player_seek_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    position = await cg.templatable(config[CONF_POSITION], args, float)
    cg.add(var.set_position(position))
    cg.add(var.set_relative(config[CONF_RELATIVE]))
    return var


@automation.register_action(
    "simple_video_player.stop",
    StopAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,  # just sets the atomic state and returns
)
async def simple_video_player_stop_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.channels.refresh",
    ChannelRefreshAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,  # queues the request for the channel task
)
async def simple_video_player_channels_refresh_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.channels.play",
    ChannelPlayAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,  # queues the request for the channel task
)
async def simple_video_player_channels_play_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.channels.stop",
    ChannelStopAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,  # queues the request for the channel task
)
async def simple_video_player_channels_stop_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


# As Portall's portall.sleep / portall.wake: the stream sender stops drawing for a panel nobody can
# see (backlight off) and redraws once it is awake again.
@automation.register_action(
    "simple_video_player.stream.sleep",
    StreamSleepAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,
)
async def simple_video_player_stream_sleep_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "simple_video_player.stream.wake",
    StreamWakeAction,
    SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
    synchronous=True,
)
async def simple_video_player_stream_wake_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


# The output a stream is made for; a connected sender is told at once and restarts for it.
@automation.register_action(
    "simple_video_player.stream.output",
    StreamOutputAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Required(CONF_FULLSCREEN): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def simple_video_player_stream_output_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_fullscreen(await cg.templatable(config[CONF_FULLSCREEN], args, bool)))
    return var


# The volume of what this player plays: the speaker's own set_volume() (a template number can drive it).
@automation.register_action(
    "simple_video_player.set_volume",
    SetVolumeAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Required(CONF_VOLUME): cv.templatable(cv.percentage),
        }
    ),
    synchronous=True,
)
async def simple_video_player_set_volume_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    volume = await cg.templatable(config[CONF_VOLUME], args, float)
    cg.add(var.set_volume(volume))
    return var


# Pictures (picture_canvas_id). show_image: index or path (a path outside the folder opens its folder).
_PICTURE_TARGET_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Optional(CONF_INDEX): cv.templatable(cv.int_),
            cv.Optional(CONF_PATH): cv.templatable(cv.string),
        }
    ),
    cv.has_exactly_one_key(CONF_INDEX, CONF_PATH),
)


async def _picture_target_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    if CONF_PATH in config:
        cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    else:
        cg.add(var.set_index(await cg.templatable(config[CONF_INDEX], args, cg.int_)))
    return var


@automation.register_action(
    "simple_video_player.show_image", ShowImageAction, _PICTURE_TARGET_SCHEMA, synchronous=True
)
async def simple_video_player_show_image_to_code(config, action_id, template_arg, args):
    return await _picture_target_to_code(config, action_id, template_arg, args)


# index: one of picture_directories; path: any folder.
@automation.register_action(
    "simple_video_player.picture_directory", PictureDirectoryAction, _PICTURE_TARGET_SCHEMA, synchronous=True
)
async def simple_video_player_picture_directory_to_code(config, action_id, template_arg, args):
    return await _picture_target_to_code(config, action_id, template_arg, args)


def _register_picture_command(name, command):
    async def _to_code(config, action_id, template_arg, args):
        paren = await cg.get_variable(config[CONF_ID])
        return cg.new_Pvariable(action_id, template_arg, paren, command)

    automation.register_action(
        f"simple_video_player.{name}",
        PictureCommandAction,
        SIMPLE_VIDEO_PLAYER_ACTION_SCHEMA,
        synchronous=True,
    )(_to_code)


for _name, _command in (
    ("next_image", PictureCommand.NEXT),
    ("previous_image", PictureCommand.PREVIOUS),
    ("start_slideshow", PictureCommand.START),
    ("stop_slideshow", PictureCommand.STOP),
    ("pause_slideshow", PictureCommand.PAUSE),
    ("toggle_slideshow", PictureCommand.TOGGLE),
    ("refresh_images", PictureCommand.REFRESH),
):
    _register_picture_command(_name, _command)


# After the canvas widget was resized: a buffer of its size, the picture fitted again.
@automation.register_action(
    "simple_video_player.fullscreen",
    FullscreenAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SimpleVideoPlayer),
            cv.Required(CONF_FULLSCREEN): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def simple_video_player_fullscreen_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_fullscreen(await cg.templatable(config[CONF_FULLSCREEN], args, bool)))
    return var
