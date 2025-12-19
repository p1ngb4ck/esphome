from __future__ import annotations

import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components.transcoder import (
    require_bmp_decoder,
    require_jpeg_decoder,
    require_png_decoder,
)
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

CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = []
AUTO_LOAD = ["transcoder", "image"]

# Namespaces
picture_viewer_ns = cg.esphome_ns.namespace("picture_viewer")

# Classes
PictureViewer = picture_viewer_ns.class_("PictureViewer", cg.Component)

# Triggers
ThumbnailClickTrigger = picture_viewer_ns.class_(
    "ThumbnailClickTrigger", automation.Trigger.template(cg.size_t)
)

# Enums
SlideshowMode = picture_viewer_ns.enum("SlideshowMode", is_class=True)
ImageFitMode = picture_viewer_ns.enum("ImageFitMode", is_class=True)
IMAGE_FIT_MODES = {
    "SCALE_TO_FIT": ImageFitMode.SCALE_TO_FIT,
    "SCALE_TO_FILL": ImageFitMode.SCALE_TO_FILL,
    "STRETCH": ImageFitMode.STRETCH,
    "CENTER": ImageFitMode.CENTER,
}

ThumbnailLayout = picture_viewer_ns.enum("ThumbnailLayout", is_class=True)
THUMBNAIL_LAYOUTS = {
    "HORIZONTAL": ThumbnailLayout.HORIZONTAL,
    "VERTICAL": ThumbnailLayout.VERTICAL,
    "GRID": ThumbnailLayout.GRID,
}

ThumbnailSlideEdge = picture_viewer_ns.enum("ThumbnailSlideEdge", is_class=True)
THUMBNAIL_SLIDE_EDGES = {
    "LEFT": ThumbnailSlideEdge.LEFT,
    "RIGHT": ThumbnailSlideEdge.RIGHT,
    "TOP": ThumbnailSlideEdge.TOP,
    "BOTTOM": ThumbnailSlideEdge.BOTTOM,
}

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

# Configuration keys
CONF_FILE_MANAGER_ID = "file_manager_id"
CONF_CANVAS_ID = "canvas_id"
CONF_DISPLAY_ID = "display_id"
CONF_DIRECTORIES = "directories"
CONF_PATH = "path"
CONF_SLIDESHOW_INTERVAL = "slideshow_interval"
CONF_ENABLE_THUMBNAILS = "enable_thumbnails"
CONF_THUMBNAIL_WIDTH = "thumbnail_width"
CONF_THUMBNAIL_HEIGHT = "thumbnail_height"
CONF_THUMBNAIL_MAX_COUNT = "thumbnail_max_count"
CONF_THUMBNAIL_DECODE_WORK_BUFFER_SIZE = "thumbnail_decode_work_buffer_size"
CONF_THUMBNAIL_LAZY_LOAD = "thumbnail_lazy_load"
CONF_THUMBNAIL_PRELOAD_COUNT = "thumbnail_preload_count"
CONF_THUMBNAIL_LAYOUT = "thumbnail_layout"
CONF_THUMBNAIL_SPACING = "thumbnail_spacing"
CONF_THUMBNAIL_GRID_COLUMNS = "thumbnail_grid_columns"
CONF_THUMBNAIL_CONTAINER_ID = "thumbnail_container_id"
CONF_FIT_MODE = "fit_mode"
CONF_JPEG_RGB_ORDER = "jpeg_rgb_order"
CONF_JPEG_COLOR_SPACE = "jpeg_color_space"
CONF_JPEG_OUTPUT_FORMAT = "jpeg_output_format"
CONF_OVERLAY_ICON_SIZE = "overlay_icon_size"
CONF_OVERLAY_ICON_COLOR = "overlay_icon_color"
CONF_OVERLAY_DURATION = "overlay_duration"
CONF_THUMBNAIL_STYLE_ID = "thumbnail_style_id"
CONF_THUMBNAIL_ACTIVE_STYLE_ID = "thumbnail_active_style_id"
CONF_THUMBNAIL_CONTAINER_STYLE_ID = "thumbnail_container_style_id"
CONF_THUMBNAIL_LABEL_PATTERN = "thumbnail_label_pattern"
CONF_THUMBNAIL_LABEL_STYLE_ID = "thumbnail_label_style_id"
CONF_DEFAULT_IMAGE_INDEX = "default_image_index"
CONF_ON_THUMBNAIL_CLICK = "on_thumbnail_click"
CONF_LONG_PRESS_TIME = "long_press_time"
CONF_THUMBNAIL_SLIDE_EDGE = "thumbnail_slide_edge"
CONF_THUMBNAIL_SLIDE_ENABLED = "thumbnail_slide_enabled"
CONF_THUMBNAIL_AUTO_HIDE_TIMEOUT = "thumbnail_auto_hide_timeout"

# Directory configuration schema
DIRECTORY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_PATH): cv.string,
        cv.Optional(CONF_JPEG_RGB_ORDER, default="RGB"): cv.enum(
            JPEG_RGB_ORDER, upper=True
        ),
        cv.Optional(CONF_JPEG_COLOR_SPACE, default="BT601"): cv.enum(
            JPEG_COLOR_SPACE, upper=True
        ),
        cv.Optional(CONF_JPEG_OUTPUT_FORMAT, default="RGB565"): cv.enum(
            JPEG_OUTPUT_FORMAT, upper=True
        ),
    }
)


def validate_buffer_sizes(config):
    """Validate that buffer sizes fit within reasonable memory limits"""
    # Calculate thumbnail buffer array size
    thumb_width = config[CONF_THUMBNAIL_WIDTH]
    thumb_height = config[CONF_THUMBNAIL_HEIGHT]
    max_count = config[CONF_THUMBNAIL_MAX_COUNT]
    bytes_per_pixel = 2  # RGB565

    thumbnail_buffer_size = thumb_width * thumb_height * bytes_per_pixel * max_count
    work_buffer_size = config[CONF_THUMBNAIL_DECODE_WORK_BUFFER_SIZE]

    total_memory = thumbnail_buffer_size + work_buffer_size

    # Warn if total memory exceeds 4MB (reasonable PSRAM limit for thumbnails)
    if total_memory > 4 * 1024 * 1024:
        _LOGGER.warning(
            "Thumbnail buffers require %.2fMB "
            "(%.2fMB for %d thumbnails + %.2fMB work buffer). "
            "Ensure your device has sufficient PSRAM.",
            total_memory / (1024 * 1024),
            thumbnail_buffer_size / (1024 * 1024),
            max_count,
            work_buffer_size / (1024 * 1024),
        )

    # Error if work buffer is smaller than a single thumbnail
    min_work_buffer = thumb_width * thumb_height * bytes_per_pixel
    if work_buffer_size < min_work_buffer:
        raise cv.Invalid(
            f"Work buffer size ({work_buffer_size} bytes) is too small for "
            f"thumbnail size {thumb_width}x{thumb_height} "
            f"(requires at least {min_work_buffer} bytes)"
        )

    return config


# Component configuration
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PictureViewer),
            cv.Optional(CONF_FILE_MANAGER_ID): cv.use_id(cg.Component),  # FileManager
            cv.Optional(CONF_CANVAS_ID): cv.use_id(lv_canvas_t)
            if LVGL_AVAILABLE
            else cv.invalid("LVGL not available"),  # LVGL Canvas widget
            cv.Optional(CONF_DISPLAY_ID): cv.use_id(cg.Component),  # Display
            cv.Required(CONF_DIRECTORIES): cv.All(
                cv.ensure_list(DIRECTORY_SCHEMA), cv.Length(min=1)
            ),
            cv.Optional(
                CONF_SLIDESHOW_INTERVAL, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ENABLE_THUMBNAILS, default=True): cv.boolean,
            cv.Optional(CONF_THUMBNAIL_WIDTH, default=120): cv.int_range(
                min=32, max=320
            ),
            cv.Optional(CONF_THUMBNAIL_HEIGHT, default=90): cv.int_range(
                min=32, max=240
            ),
            cv.Optional(CONF_THUMBNAIL_MAX_COUNT, default=10): cv.int_range(
                min=1, max=50
            ),
            cv.Optional(
                CONF_THUMBNAIL_DECODE_WORK_BUFFER_SIZE, default=2097152
            ): cv.int_range(
                min=262144  # Min 256KB for JPEG decode
            ),
            cv.Optional(CONF_THUMBNAIL_LAZY_LOAD, default=True): cv.boolean,
            cv.Optional(CONF_THUMBNAIL_PRELOAD_COUNT, default=5): cv.int_range(
                min=0, max=50
            ),
            cv.Optional(CONF_THUMBNAIL_LAYOUT, default="HORIZONTAL"): cv.enum(
                THUMBNAIL_LAYOUTS, upper=True
            ),
            cv.Optional(CONF_THUMBNAIL_SPACING, default=5): cv.int_range(min=0, max=50),
            cv.Optional(CONF_THUMBNAIL_GRID_COLUMNS, default=3): cv.int_range(
                min=1, max=10
            ),
            cv.Optional(CONF_THUMBNAIL_CONTAINER_ID): cv.use_id(
                cg.global_ns.class_("lv_obj_t")
            )
            if LVGL_AVAILABLE
            else cv.invalid("LVGL not available"),
            cv.Optional(CONF_FIT_MODE, default="SCALE_TO_FIT"): cv.enum(
                IMAGE_FIT_MODES, upper=True
            ),
            cv.Optional(CONF_OVERLAY_ICON_SIZE, default=128): cv.int_range(
                min=32, max=256
            ),
            cv.Optional(CONF_OVERLAY_ICON_COLOR, default=0xFFFFFF): cv.hex_uint32_t,
            cv.Optional(CONF_OVERLAY_DURATION, default=1500): cv.int_range(
                min=500, max=5000
            ),
            cv.Optional(CONF_THUMBNAIL_STYLE_ID): cv.use_id(
                cg.global_ns.class_("lv_style_t")
            )
            if LVGL_AVAILABLE
            else cv.invalid("LVGL not available"),
            cv.Optional(CONF_THUMBNAIL_ACTIVE_STYLE_ID): cv.use_id(
                cg.global_ns.class_("lv_style_t")
            )
            if LVGL_AVAILABLE
            else cv.invalid("LVGL not available"),
            cv.Optional(CONF_THUMBNAIL_CONTAINER_STYLE_ID): cv.use_id(
                cg.global_ns.class_("lv_style_t")
            )
            if LVGL_AVAILABLE
            else cv.invalid("LVGL not available"),
            cv.Optional(CONF_THUMBNAIL_LABEL_PATTERN): cv.string,
            cv.Optional(CONF_THUMBNAIL_LABEL_STYLE_ID): cv.use_id(
                cg.global_ns.class_("lv_style_t")
            )
            if LVGL_AVAILABLE
            else cv.invalid("LVGL not available"),
            cv.Optional(CONF_DEFAULT_IMAGE_INDEX): cv.Any(
                cv.int_range(min=0), cv.one_of("off", "OFF", lower=True)
            ),
            cv.Optional(
                CONF_LONG_PRESS_TIME, default="1000ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_THUMBNAIL_SLIDE_EDGE, default="RIGHT"): cv.enum(
                THUMBNAIL_SLIDE_EDGES, upper=True
            ),
            cv.Optional(CONF_THUMBNAIL_SLIDE_ENABLED, default=False): cv.boolean,
            cv.Optional(
                CONF_THUMBNAIL_AUTO_HIDE_TIMEOUT, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_THUMBNAIL_CLICK): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        ThumbnailClickTrigger
                    ),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_buffer_sizes,
)


async def to_code(config):
    """Generate code for picture_viewer component"""
    # Ensure decoder requirements are asserted during code generation
    require_jpeg_decoder()  # Picture viewer supports JPEG images
    require_png_decoder()  # Picture viewer supports PNG images
    require_bmp_decoder()  # Picture viewer supports BMP images

    # Set defines FIRST (before component registration)
    # Transcoder is always loaded via AUTO_LOAD and sets decoder defines globally
    cg.add_define("USE_TRANSCODER")

    # Add conditional defines based on configuration
    if CONF_FILE_MANAGER_ID in config:
        cg.add_define("USE_STORAGE")

    if CONF_CANVAS_ID in config:
        cg.add_define("USE_LVGL")

    # Create and register component AFTER all defines are set
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Link file manager if provided
    if CONF_FILE_MANAGER_ID in config:
        fm = await cg.get_variable(config[CONF_FILE_MANAGER_ID])
        cg.add(var.set_file_manager(fm))

    # Link LVGL canvas if provided
    if CONF_CANVAS_ID in config:
        canvas = await cg.get_variable(config[CONF_CANVAS_ID])
        # Store canvas ID for debugging
        cg.add(var.set_canvas_id(str(config[CONF_CANVAS_ID])))
        # Set the canvas object pointer (canvas is already lv_obj_t*)
        cg.add(var.set_canvas(canvas))

    # Link display if provided
    if CONF_DISPLAY_ID in config:
        display = await cg.get_variable(config[CONF_DISPLAY_ID])
        cg.add(var.set_display(display))

    # Link thumbnail container if provided
    if CONF_THUMBNAIL_CONTAINER_ID in config:
        container = await cg.get_variable(config[CONF_THUMBNAIL_CONTAINER_ID])
        cg.add(var.set_thumbnail_container(container))

    # Link thumbnail styles if provided
    if CONF_THUMBNAIL_STYLE_ID in config:
        style = await cg.get_variable(config[CONF_THUMBNAIL_STYLE_ID])
        cg.add(var.set_thumbnail_style(style))

    if CONF_THUMBNAIL_ACTIVE_STYLE_ID in config:
        active_style = await cg.get_variable(config[CONF_THUMBNAIL_ACTIVE_STYLE_ID])
        cg.add(var.set_thumbnail_active_style(active_style))

    if CONF_THUMBNAIL_CONTAINER_STYLE_ID in config:
        container_style = await cg.get_variable(
            config[CONF_THUMBNAIL_CONTAINER_STYLE_ID]
        )
        cg.add(var.set_thumbnail_container_style(container_style))

    # Link thumbnail label configuration if provided
    if CONF_THUMBNAIL_LABEL_PATTERN in config:
        cg.add(var.set_thumbnail_label_pattern(config[CONF_THUMBNAIL_LABEL_PATTERN]))

    if CONF_THUMBNAIL_LABEL_STYLE_ID in config:
        label_style = await cg.get_variable(config[CONF_THUMBNAIL_LABEL_STYLE_ID])
        cg.add(var.set_thumbnail_label_style(label_style))

    # Default image index configuration
    if CONF_DEFAULT_IMAGE_INDEX in config:
        default_index = config[CONF_DEFAULT_IMAGE_INDEX]
        if isinstance(default_index, str) and default_index.lower() == "off":
            cg.add(var.set_default_image_index(-1))  # -1 means disabled
        else:
            cg.add(var.set_default_image_index(default_index))

    # Thumbnail click automation
    for conf in config.get(CONF_ON_THUMBNAIL_CLICK, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.size_t, "x")], conf)
        cg.add(var.add_on_thumbnail_click_callback(trigger))

    # Configuration
    cg.add(var.set_slideshow_interval(config[CONF_SLIDESHOW_INTERVAL]))
    cg.add(var.set_enable_thumbnails(config[CONF_ENABLE_THUMBNAILS]))
    cg.add(var.set_thumbnail_width(config[CONF_THUMBNAIL_WIDTH]))
    cg.add(var.set_thumbnail_height(config[CONF_THUMBNAIL_HEIGHT]))
    cg.add(var.set_thumbnail_max_count(config[CONF_THUMBNAIL_MAX_COUNT]))
    cg.add(
        var.set_thumbnail_decode_work_buffer_size(
            config[CONF_THUMBNAIL_DECODE_WORK_BUFFER_SIZE]
        )
    )
    cg.add(var.set_thumbnail_lazy_load(config[CONF_THUMBNAIL_LAZY_LOAD]))
    cg.add(var.set_thumbnail_preload_count(config[CONF_THUMBNAIL_PRELOAD_COUNT]))
    cg.add(var.set_thumbnail_layout(config[CONF_THUMBNAIL_LAYOUT]))
    cg.add(var.set_thumbnail_spacing(config[CONF_THUMBNAIL_SPACING]))
    cg.add(var.set_thumbnail_grid_columns(config[CONF_THUMBNAIL_GRID_COLUMNS]))
    cg.add(var.set_fit_mode(config[CONF_FIT_MODE]))
    cg.add(var.set_overlay_icon_size(config[CONF_OVERLAY_ICON_SIZE]))
    cg.add(var.set_overlay_icon_color(config[CONF_OVERLAY_ICON_COLOR]))
    cg.add(var.set_overlay_duration(config[CONF_OVERLAY_DURATION]))
    cg.add(var.set_long_press_time(config[CONF_LONG_PRESS_TIME]))
    cg.add(var.set_thumbnail_slide_edge(config[CONF_THUMBNAIL_SLIDE_EDGE]))
    cg.add(var.set_thumbnail_slide_enabled(config[CONF_THUMBNAIL_SLIDE_ENABLED]))
    cg.add(
        var.set_thumbnail_auto_hide_timeout(config[CONF_THUMBNAIL_AUTO_HIDE_TIMEOUT])
    )

    # Add directories with per-directory JPEG decoder configuration
    for dir_config in config[CONF_DIRECTORIES]:
        cg.add(
            var.add_directory(
                dir_config[CONF_PATH],
                dir_config[CONF_JPEG_RGB_ORDER],
                dir_config[CONF_JPEG_COLOR_SPACE],
                dir_config[CONF_JPEG_OUTPUT_FORMAT],
            )
        )

    # Link to transcoder component (handles all decoder initialization)
    # The transcoder dependency ensures it's initialized before picture_viewer
    cg.add(
        var.set_transcoder(cg.RawExpression("esphome::transcoder::global_transcoder"))
    )

    _LOGGER.info("Picture viewer configured to use transcoder component")

    return var
