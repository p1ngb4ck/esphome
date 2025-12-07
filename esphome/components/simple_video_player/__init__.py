from __future__ import annotations

import logging

import esphome.codegen as cg
from esphome.components.transcoder import require_jpeg_decoder
import esphome.config_validation as cv
from esphome.const import CONF_ID

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
SimpleVideoViewer = simple_video_player_ns.class_("SimpleVideoViewer", cg.Component)

CONF_CANVAS_ID = "canvas_id"

# JPEG decoder configuration enums
JPEG_RGB_ORDER = {
    "RGB": 0,  # JPEG_DEC_RGB_ELEMENT_ORDER_RGB
    "BGR": 1,  # JPEG_DEC_RGB_ELEMENT_ORDER_BGR
}

JPEG_COLOR_SPACE = {
    "BT601": 0,
    "BT709": 1,
}

JPEG_OUTPUT_FORMAT = {
    "RGB888": 0x02000000,  # COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB888)
    "RGB565": 0x02000002,  # COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565)
    "GRAY": 0x03000000,  # COLOR_TYPE_ID(COLOR_SPACE_GRAY, COLOR_PIXEL_GRAY8)
}


# Component configuration
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SimpleVideoViewer),
            cv.Required(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
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
    canvas = (await cg.get_variable(config[CONF_CANVAS_ID]),)
    cg.add(var.set_canvas(canvas))

    # Link to transcoder component (handles all decoder initialization)
    # The transcoder dependency ensures it's initialized before picture_viewer
    cg.add(
        var.set_transcoder(cg.RawExpression("esphome::transcoder::global_transcoder"))
    )
