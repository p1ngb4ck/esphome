import esphome.codegen as cg
from esphome.components.display import get_display_metadata
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ROTATION
from esphome.core import ID
import esphome.final_validate as fv

from .defines import CONF_DISPLAYS, CONF_LVGL_ID
from .lvcode import LvglComponent
from .widgets.canvas import CONF_EXTERNAL_BUFFER, lv_canvas_t

CONF_CANVASES = "canvases"
CONF_MIN_FPS = "min_fps"
CONF_VIDEO_OUTPUT = "video_output"
CONF_VIDEO_OUTPUT_ID = "video_output_id"

display_ns = cg.esphome_ns.namespace("display")
VideoOutput = display_ns.class_("VideoOutput")
VideoCanvas = display_ns.class_("VideoCanvas")

LVGL_VIDEO_OUTPUT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VideoOutput),
        cv.Required(CONF_CANVASES): cv.All(
            cv.ensure_list(cv.use_id(lv_canvas_t)), cv.Length(min=1, max=2)
        ),
        cv.Optional(CONF_MIN_FPS, default=25): cv.int_range(min=1, max=120),
    }
)

VIDEO_OUTPUT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_LVGL_ID): cv.use_id(LvglComponent),
    }
)


def screen_size(lvgl_id) -> tuple[int, int]:
    fconf = fv.full_config.get()
    lvgl = fconf.get_config_for_path(fconf.get_path_for_id(lvgl_id)[:-1])
    width = 0
    height = 0
    for display_id in lvgl.get(CONF_DISPLAYS, []):
        meta = get_display_metadata(display_id)
        width = max(width, meta.width)
        height = max(height, meta.height)
    if int(lvgl.get(CONF_ROTATION, 0)) in (90, 270):
        width, height = height, width
    return width, height


def final_validate_video_output(config):
    if CONF_LVGL_ID not in config:
        return config
    fconf = fv.full_config.get()
    lvgl = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_LVGL_ID])[:-1])
    output = lvgl.get(CONF_VIDEO_OUTPUT)
    if output is None:
        raise cv.Invalid(f"lvgl '{config[CONF_LVGL_ID]}' has no {CONF_VIDEO_OUTPUT} (its canvases)")
    own = fconf.get_config_for_path(fconf.get_path_for_id(config[CONF_ID])[:-1])
    own[CONF_VIDEO_OUTPUT_ID] = output[CONF_ID]
    for canvas_id in output[CONF_CANVASES]:
        try:
            canvas = fconf.get_config_for_path(fconf.get_path_for_id(canvas_id)[:-1])
        except KeyError as err:
            raise cv.Invalid(f"Could not resolve canvas '{canvas_id}'") from err
        if not canvas.get(CONF_EXTERNAL_BUFFER, False):
            raise cv.Invalid(
                f"canvas '{canvas_id}' needs 'external_buffer: true': the video brings its own buffer"
            )
    return config


async def video_output_to_code(var, config):
    if CONF_LVGL_ID not in config:
        return
    cg.add(var.set_video_output(await cg.get_variable(config[CONF_VIDEO_OUTPUT_ID])))


async def lvgl_video_output_to_code(lv_component, display, config):
    output = cg.new_Pvariable(config[CONF_ID])
    for index, canvas_id in enumerate(config[CONF_CANVASES]):
        canvas = cg.new_Pvariable(
            ID(f"{config[CONF_ID].id}_canvas_{index}", is_declaration=True, type=VideoCanvas)
        )
        cg.add(canvas.set_lvgl(lv_component))
        cg.add(canvas.set_display(display))
        cg.add(canvas.set_min_fps(config[CONF_MIN_FPS]))
        cg.add(canvas.set_canvas(await cg.get_variable(canvas_id)))
        cg.add(output.add_canvas(canvas))
