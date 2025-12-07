from __future__ import annotations

import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components.transcoder import require_jpeg_decoder
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

_LOGGER = logging.getLogger(__name__)

# Import LVGL canvas type for proper widget ID validation
try:
    from esphome.components.lvgl.widgets.canvas import lv_canvas_t

    LVGL_AVAILABLE = True
except ImportError:
    LVGL_AVAILABLE = False
    lv_canvas_t = None

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = []
AUTO_LOAD = ["transcoder", "image"]

# Namespaces
simple_video_player_ns = cg.esphome_ns.namespace("simple_video_player")

# Classes
SimpleVideoPlayer = simple_video_player_ns.class_("SimpleVideoPlayer", cg.Component)

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
CONF_CACHE_BUFFER_SIZE = "cache_buffer_size"
CONF_INPUT_BUFFER_SIZE = "input_buffer_size"
CONF_TARGET_FPS = "target_fps"
CONF_ON_PLAYBACK_STARTED = "on_playback_started"
CONF_ON_PLAYBACK_FINISHED = "on_playback_finished"
CONF_ON_PLAYBACK_PAUSED = "on_playback_paused"
CONF_ON_PLAYBACK_ERROR = "on_playback_error"

# Default values
DEFAULT_CACHE_BUFFER_SIZE = 16 * 1024  # 16KB
DEFAULT_INPUT_BUFFER_SIZE = 256 * 1024  # 256KB
DEFAULT_TARGET_FPS = 30.0

# Validation ranges
MIN_CACHE_BUFFER_SIZE = 8 * 1024  # 8KB
MAX_CACHE_BUFFER_SIZE = 16 * 1024  # 16KB
MIN_INPUT_BUFFER_SIZE = 128 * 1024  # 128KB
MAX_INPUT_BUFFER_SIZE = 2 * 1024 * 1024  # 2MB
MIN_FPS = 1.0
MAX_FPS = 60.0


# Component configuration
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SimpleVideoPlayer),
            cv.Required(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
            cv.Optional(
                CONF_CACHE_BUFFER_SIZE, default=DEFAULT_CACHE_BUFFER_SIZE
            ): cv.int_range(min=MIN_CACHE_BUFFER_SIZE, max=MAX_CACHE_BUFFER_SIZE),
            cv.Optional(
                CONF_INPUT_BUFFER_SIZE, default=DEFAULT_INPUT_BUFFER_SIZE
            ): cv.int_range(min=MIN_INPUT_BUFFER_SIZE, max=MAX_INPUT_BUFFER_SIZE),
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


async def to_code(config):
    require_jpeg_decoder()  # Simple Video Player only needs JPEG decoder
    cg.add_define("USE_TRANSCODER")
    cg.add_define("USE_STORAGE")
    cg.add_define("USE_LVGL")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Set canvas
    canvas = await cg.get_variable(config[CONF_CANVAS_ID])
    cg.add(var.set_canvas(canvas))

    # Set buffer sizes
    cg.add(var.set_cache_buffer_size(config[CONF_CACHE_BUFFER_SIZE]))
    cg.add(var.set_input_buffer_size(config[CONF_INPUT_BUFFER_SIZE]))

    # Set target FPS
    cg.add(var.set_target_fps(config[CONF_TARGET_FPS]))

    # Link to transcoder component (handles all decoder initialization)
    # The transcoder dependency ensures it's initialized before simple_video_player
    cg.add(
        var.set_transcoder(cg.RawExpression("esphome::transcoder::global_transcoder"))
    )

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
