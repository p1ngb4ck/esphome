"""Transcoder component for managing hardware media codecs (JPEG, H.264, etc.)."""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = []

# Codec requirement keys
CODEC_JPEG_DECODER = "jpeg_decoder"
CODEC_JPEG_ENCODER = "jpeg_encoder"
CODEC_PNG_DECODER = "png_decoder"
CODEC_BMP_DECODER = "bmp_decoder"
CODEC_H264_DECODER = "h264_decoder"
CODEC_H264_ENCODER = "h264_encoder"

transcoder_ns = cg.esphome_ns.namespace("transcoder")
Transcoder = transcoder_ns.class_("Transcoder", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Transcoder),
    }
).extend(cv.COMPONENT_SCHEMA)


# ========== Codec Requirement Registration API ==========


def _get_requirements() -> set:
    """Get or create transcoder requirements set in CORE.data."""
    return CORE.data.setdefault("transcoder_requirements", set())


def require_jpeg_decoder():
    """Register requirement for JPEG decoder."""
    reqs = _get_requirements()
    reqs.add(CODEC_JPEG_DECODER)
    _LOGGER.debug("Transcoder: JPEG decoder required")


def require_jpeg_encoder():
    """Register requirement for JPEG encoder."""
    reqs = _get_requirements()
    reqs.add(CODEC_JPEG_ENCODER)
    _LOGGER.debug("Transcoder: JPEG encoder required")


def require_png_decoder():
    """Register requirement for PNG decoder."""
    reqs = _get_requirements()
    reqs.add(CODEC_PNG_DECODER)
    _LOGGER.debug("Transcoder: PNG decoder required")


def require_bmp_decoder():
    """Register requirement for BMP decoder."""
    reqs = _get_requirements()
    reqs.add(CODEC_BMP_DECODER)
    _LOGGER.debug("Transcoder: BMP decoder required")


def require_h264_decoder():
    """Register requirement for H.264 decoder."""
    reqs = _get_requirements()
    reqs.add(CODEC_H264_DECODER)
    _LOGGER.debug("Transcoder: H.264 decoder required")


def require_h264_encoder():
    """Register requirement for H.264 encoder."""
    reqs = _get_requirements()
    reqs.add(CODEC_H264_ENCODER)
    _LOGGER.debug("Transcoder: H.264 encoder required")


# ========== Code Generation ==========


async def to_code(config):
    """Configure transcoder component based on platform and requirements."""
    # Get registered codec requirements
    requirements = CORE.data.get("transcoder_requirements", set())
    if not requirements:
        _LOGGER.warning(
            "Transcoder loaded but no codecs required by any component. "
            "This wastes resources."
        )
        return

    _LOGGER.info("Transcoder requirements: %s", ", ".join(sorted(requirements)))

    # Create and register component
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Global transcoder flag
    cg.add_define("USE_TRANSCODER")

    # Check what codecs are needed
    jpeg_needed = (
        CODEC_JPEG_DECODER in requirements or CODEC_JPEG_ENCODER in requirements
    )
    png_needed = CODEC_PNG_DECODER in requirements
    bmp_needed = CODEC_BMP_DECODER in requirements
    h264_needed = (
        CODEC_H264_DECODER in requirements or CODEC_H264_ENCODER in requirements
    )

    # Configure codecs based on platform
    if CORE.is_esp32:
        from esphome.components.esp32 import add_idf_component, get_esp32_variant

        variant = get_esp32_variant()

        # JPEG codec configuration
        if jpeg_needed:
            if "S2" in variant or "S3" in variant:
                # ESP32-S2/S3: esp_new_jpeg component (optimized with SIMD)
                add_idf_component(name="espressif/esp_new_jpeg", ref="1.0.0")
                if CODEC_JPEG_DECODER in requirements:
                    cg.add_define("USE_ESP_NEW_JPEG_DECODER")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_DECODER")
                if CODEC_JPEG_ENCODER in requirements:
                    cg.add_define("USE_ESP_NEW_JPEG_ENCODER")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_ENCODER")
                cg.add_define("TRANSCODER_JPEG_AVAILABLE")
                _LOGGER.info("Enabled esp_new_jpeg codec for %s", variant)

            elif "P4" in variant:
                # ESP32-P4: Hardware JPEG codec
                if CODEC_JPEG_DECODER in requirements:
                    cg.add_define("USE_HARDWARE_JPEG_DECODER")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_DECODER")
                if CODEC_JPEG_ENCODER in requirements:
                    cg.add_define("USE_HARDWARE_JPEG_ENCODER")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_ENCODER")
                cg.add_define("TRANSCODER_JPEG_AVAILABLE")
                _LOGGER.info("Enabled hardware JPEG codec for %s", variant)

            else:
                # Other ESP32 variants: JPEGDec fallback
                if CODEC_JPEG_DECODER in requirements:
                    cg.add_library("bodmer/JPEGDecoder", "1.8.0")
                    cg.add_define("USE_JPEGDEC")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_DECODER")
                    _LOGGER.info("Using JPEGDec library for %s", variant)
                if CODEC_JPEG_ENCODER in requirements:
                    _LOGGER.warning("JPEG encoder not available on %s", variant)

        # PNG codec configuration
        if png_needed:
            # PNG decoder: pngle library (platform-independent)
            cg.add_library("pngle", "1.1.0")
            cg.add_define("USE_PNG_DECODER")
            cg.add_define("TRANSCODER_ENABLE_PNG_DECODER")
            _LOGGER.info("Enabled PNG decoder (pngle library)")

        # BMP codec configuration
        if bmp_needed:
            # BMP decoder: no external library needed (simple format)
            cg.add_define("USE_BMP_DECODER")
            cg.add_define("TRANSCODER_ENABLE_BMP_DECODER")
            _LOGGER.info("Enabled BMP decoder (built-in)")

        # H.264 codec configuration
        if h264_needed:
            if "P4" in variant or "S3" in variant:
                # ESP32-P4/S3: esp_h264 component
                add_idf_component(name="espressif/esp_h264", ref="1.1.2")
                if CODEC_H264_DECODER in requirements:
                    cg.add_define("USE_ESP_H264_DECODER")
                    cg.add_define("TRANSCODER_ENABLE_H264_DECODER")
                if CODEC_H264_ENCODER in requirements:
                    cg.add_define("USE_ESP_H264_ENCODER")
                    cg.add_define("TRANSCODER_ENABLE_H264_ENCODER")
                cg.add_define("TRANSCODER_H264_AVAILABLE")
                _LOGGER.info("Enabled esp_h264 codec for %s", variant)
            else:
                _LOGGER.error("H.264 codec not available on %s", variant)

    else:
        # Non-ESP32 platforms
        if jpeg_needed and CODEC_JPEG_DECODER in requirements:
            cg.add_library("bodmer/JPEGDecoder", "1.8.0")
            cg.add_define("USE_JPEGDEC")
            cg.add_define("TRANSCODER_ENABLE_JPEG_DECODER")
            _LOGGER.info("Using JPEGDec library for non-ESP32 platform")
        if jpeg_needed and CODEC_JPEG_ENCODER in requirements:
            _LOGGER.warning("JPEG encoder not available on non-ESP32 platform")
        if png_needed:
            # PNG decoder: pngle library (platform-independent)
            cg.add_library("pngle", "1.1.0")
            cg.add_define("USE_PNG_DECODER")
            cg.add_define("TRANSCODER_ENABLE_PNG_DECODER")
            _LOGGER.info("Enabled PNG decoder for non-ESP32 platform (pngle library)")
        if bmp_needed:
            # BMP decoder: no external library needed (simple format)
            cg.add_define("USE_BMP_DECODER")
            cg.add_define("TRANSCODER_ENABLE_BMP_DECODER")
            _LOGGER.info("Enabled BMP decoder for non-ESP32 platform (built-in)")
        if h264_needed:
            _LOGGER.error("H.264 codec not available on non-ESP32 platform")
