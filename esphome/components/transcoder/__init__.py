"""Transcoder component for managing hardware media codecs (JPEG, H.264, etc.)."""
import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import (
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32P4,
    add_idf_component,
    get_esp32_variant,
)
from esphome.const import CONF_ID
from esphome.core import CORE

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = []

# Codec requirement keys
CODEC_JPEG_DECODER = "jpeg_decoder"
CODEC_JPEG_ENCODER = "jpeg_encoder"
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
# Components call these functions to declare their codec needs


def _get_requirements() -> set:
    """Get or create transcoder requirements set in CORE.data."""
    return CORE.data.setdefault("transcoder_requirements", set())


def require_jpeg_decoder():
    """
    Register requirement for JPEG decoder.
    Call this from component's __init__.py at module level (before to_code).
    """
    reqs = _get_requirements()
    reqs.add(CODEC_JPEG_DECODER)
    _LOGGER.debug("Transcoder: JPEG decoder required")


def require_jpeg_encoder():
    """
    Register requirement for JPEG encoder.
    Call this from component's __init__.py at module level (before to_code).
    """
    reqs = _get_requirements()
    reqs.add(CODEC_JPEG_ENCODER)
    _LOGGER.debug("Transcoder: JPEG encoder required")


def require_h264_decoder():
    """
    Register requirement for H.264 decoder.
    Call this from component's __init__.py at module level (before to_code).
    """
    reqs = _get_requirements()
    reqs.add(CODEC_H264_DECODER)
    _LOGGER.debug("Transcoder: H.264 decoder required")


def require_h264_encoder():
    """
    Register requirement for H.264 encoder.
    Call this from component's __init__.py at module level (before to_code).
    """
    reqs = _get_requirements()
    reqs.add(CODEC_H264_ENCODER)
    _LOGGER.debug("Transcoder: H.264 encoder required")


async def to_code(config):
    """Configure transcoder component based on platform and requirements."""
    # Get registered codec requirements
    requirements = CORE.data.get("transcoder_requirements", set())
    if not requirements:
        _LOGGER.warning(
            "Transcoder loaded but no codecs required by any component. "
            "This wastes resources. Components should call require_*() functions."
        )
        return

    _LOGGER.info("Transcoder requirements: %s", ", ".join(sorted(requirements)))

    # Create and register component FIRST (like old storage_host pattern)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Set global transcoder accessor flag
    cg.add_define("USE_TRANSCODER")

    # Determine if any JPEG codec is needed
    jpeg_needed = CODEC_JPEG_DECODER in requirements or CODEC_JPEG_ENCODER in requirements

    # Determine if any H.264 codec is needed
    h264_needed = CODEC_H264_DECODER in requirements or CODEC_H264_ENCODER in requirements

    # Configure ESP32 platform-specific codecs
    if CORE.is_esp32:
        variant = get_esp32_variant()

        # ========== JPEG Codec Support ==========
        if jpeg_needed:
            if variant in (VARIANT_ESP32S2, VARIANT_ESP32S3):
                # ESP32-S2/S3: Use esp_jpeg from ESP Component Registry
                add_idf_component(name="espressif/esp_jpeg", ref="1.3.1")
                if CODEC_JPEG_DECODER in requirements:
                    cg.add_define("USE_ESP_JPEG_DECODER")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_DECODER")
                if CODEC_JPEG_ENCODER in requirements:
                    cg.add_define("USE_ESP_JPEG_ENCODER")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_ENCODER")
                cg.add_define("TRANSCODER_JPEG_AVAILABLE")
                _LOGGER.info("Enabled esp_jpeg codec v1.3.1 for %s", variant)

            elif variant == VARIANT_ESP32P4:
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
                # Fallback: JPEGDec library for all other ESP32 variants
                if CODEC_JPEG_DECODER in requirements:
                    cg.add_library("bodmer/JPEGDecoder", "1.8.0")
                    cg.add_define("USE_JPEGDEC")
                    cg.add_define("TRANSCODER_ENABLE_JPEG_DECODER")
                    _LOGGER.info("Using JPEGDec library for %s", variant)
                if CODEC_JPEG_ENCODER in requirements:
                    _LOGGER.warning("JPEG encoder not available on %s - no fallback library", variant)

        # ========== H.264 Codec Support ==========
        if h264_needed:
            if variant in (VARIANT_ESP32P4, VARIANT_ESP32S3):
                # ESP32-P4: Hardware H.264 encoder + Software decoder
                # ESP32-S3: Software H.264 encoder/decoder
                add_idf_component(name="espressif/esp_h264", ref="1.1.2")

                if CODEC_H264_DECODER in requirements:
                    cg.add_define("USE_ESP_H264_DECODER")
                    cg.add_define("TRANSCODER_ENABLE_H264_DECODER")
                if CODEC_H264_ENCODER in requirements:
                    cg.add_define("USE_ESP_H264_ENCODER")
                    cg.add_define("TRANSCODER_ENABLE_H264_ENCODER")
                cg.add_define("TRANSCODER_H264_AVAILABLE")

                hw_type = "Hardware (P4)" if variant == VARIANT_ESP32P4 else "Software (S3)"
                _LOGGER.info("Enabled esp_h264 codec v1.1.2 for %s - %s", variant, hw_type)
            else:
                _LOGGER.error(
                    "H.264 codec requested but only available on ESP32-P4/S3 (current: %s)", variant
                )

    else:
        # Non-ESP32 platforms: Use JPEGDec fallback
        if jpeg_needed and CODEC_JPEG_DECODER in requirements:
            cg.add_library("bodmer/JPEGDecoder", "1.8.0")
            cg.add_define("USE_JPEGDEC")
            cg.add_define("TRANSCODER_ENABLE_JPEG_DECODER")
            _LOGGER.info("Using JPEGDec library for non-ESP32 platform")
        if jpeg_needed and CODEC_JPEG_ENCODER in requirements:
            _LOGGER.warning("JPEG encoder not available on non-ESP32 platforms")
        if h264_needed:
            _LOGGER.error("H.264 codec not available on non-ESP32 platforms")
