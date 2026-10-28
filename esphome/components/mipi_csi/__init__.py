import logging
from pathlib import Path
import re

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import esp32, i2c
from esphome.components.camera_encoder import H264Encoder
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.components.esp32.const import VARIANT_ESP32P4
import esphome.config_validation as cv
from esphome.const import (
    CONF_FREQUENCY,
    CONF_I2C_ID,
    CONF_ID,
    CONF_PIN,
    CONF_RESET_PIN,
    CONF_RESOLUTION,
    CONF_SENSOR,
    CONF_TRIGGER_ID,
    Framework,
)
from esphome.core import CORE
from esphome.core.entity_helpers import setup_entity
from esphome.types import ConfigType

from .const import (
    CONF_EXTERNAL_CLOCK,
    CONF_FRAME_BUFFER_COUNT,
    CONF_FRAMERATE,
    CONF_HORIZONTAL_FLIP,
    CONF_INIT_LDO,
    CONF_JPEG_QUALITY,
    CONF_ON_IMAGE,
    CONF_ON_STREAM_START,
    CONF_ON_PREVIEW_FRAME,
    CONF_ON_PREVIEW_RELEASED,
    CONF_ON_STREAM_STOP,
    CONF_PIXEL_FORMAT,
    CONF_POWER_PIN,
    CONF_SCCB_FREQUENCY,
    CONF_VERTICAL_FLIP,
)
from .sensors import SENSOR_FORMATS

CONF_H264_ENCODER = "h264_encoder"

_LOGGER = logging.getLogger(__name__)

DOMAIN = "mipi_csi"
CODEOWNERS = ["@Horstexplorer"]

DEPENDENCIES = ["esp32", "i2c", "psram"]
AUTO_LOAD = ["camera"]
MULTI_CONF = False

mipi_csi_ns = cg.esphome_ns.namespace("mipi_csi")
MipiCsiCamera = mipi_csi_ns.class_("MipiCsiCamera", cg.Component, cg.EntityBase)
CameraImageData = mipi_csi_ns.struct("CameraImageData")
MipiCsiImageTrigger = mipi_csi_ns.class_(
    "MipiCsiImageTrigger", automation.Trigger.template()
)
MipiCsiStreamStartTrigger = mipi_csi_ns.class_(
    "MipiCsiStreamStartTrigger", automation.Trigger.template()
)
MipiCsiStreamStopTrigger = mipi_csi_ns.class_(
    "MipiCsiStreamStopTrigger", automation.Trigger.template()
)
MipiCsiPreviewFrameTrigger = mipi_csi_ns.class_(
    "MipiCsiPreviewFrameTrigger", automation.Trigger.template()
)
MipiCsiPreviewReleasedTrigger = mipi_csi_ns.class_(
    "MipiCsiPreviewReleasedTrigger", automation.Trigger.template(cg.uint8.operator("ptr"))
)

PixelFormat = mipi_csi_ns.enum("PixelFormat", is_class=True)
PIXEL_FORMATS = {
    "RGB565": PixelFormat.PIXEL_FORMAT_RGB565,
    "RGB888": PixelFormat.PIXEL_FORMAT_RGB888,
    "YUV422": PixelFormat.PIXEL_FORMAT_YUV422,
    "GRAYSCALE": PixelFormat.PIXEL_FORMAT_GRAYSCALE,
}

_RESOLUTION_PATTERN = re.compile(r"^\s*(\d+)\s*[xX]\s*(\d+)\s*$")


def validate_sensor(value: str) -> str:
    """Accepts a built-in sensor, or the name of one whose driver comes from another IDF component."""
    value = cv.string_strict(value).strip().upper()
    if not value:
        raise cv.Invalid("Sensor name must not be empty")
    if value not in SENSOR_FORMATS:
        _LOGGER.warning(
            "Sensor '%s' is not built into esp_cam_sensor. Its driver must come from another "
            "IDF component (for example under esp32: framework: components:). Built-in "
            "sensors are: %s",
            value,
            ", ".join(SENSOR_FORMATS),
        )
    return value


def validate_resolution(value: str) -> tuple[int, int]:
    """Parses a `WIDTHxHEIGHT` resolution into a pair of pixel counts."""
    value = cv.string(value)
    match = _RESOLUTION_PATTERN.match(value)
    if match is None:
        raise cv.Invalid(f"Resolution must be written as WIDTHxHEIGHT, got '{value}'")
    width, height = int(match.group(1)), int(match.group(2))
    # A zero is what tells the component to keep the sensor's own resolution, and anything above
    # 65535 does not fit the size the component stores, so neither may reach it from here.
    for dimension, name in ((width, "Width"), (height, "Height")):
        if not 1 <= dimension <= 65535:
            raise cv.Invalid(
                f"{name} must be between 1 and 65535, got {dimension} in '{value}'"
            )
    return width, height


def _validate_h264_format(config: ConfigType) -> ConfigType:
    if CONF_H264_ENCODER in config and config[CONF_PIXEL_FORMAT] not in ("RGB565", "YUV422"):
        raise cv.Invalid(
            f"{CONF_H264_ENCODER} needs {CONF_PIXEL_FORMAT} RGB565 or YUV422, not {config[CONF_PIXEL_FORMAT]}"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.ENTITY_BASE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(MipiCsiCamera),
            cv.Required(CONF_SENSOR): validate_sensor,
            cv.Optional(CONF_RESOLUTION): validate_resolution,
            cv.Optional(CONF_PIXEL_FORMAT, default="RGB565"): cv.enum(
                PIXEL_FORMATS, upper=True
            ),
            cv.Optional(CONF_JPEG_QUALITY, default=40): cv.int_range(min=10, max=100),
            cv.Optional(CONF_H264_ENCODER): cv.use_id(H264Encoder),
            cv.Optional(CONF_FRAMERATE, default="10 fps"): cv.All(
                cv.framerate, cv.Range(min=1, max=60)
            ),
            # One of them is the driver's reserve (its own backup buffer is off): all from the buffer allocator
            cv.Optional(CONF_FRAME_BUFFER_COUNT, default=4): cv.int_range(min=2, max=4),
            cv.Optional(CONF_INIT_LDO, default=True): cv.boolean,
            cv.Optional(CONF_HORIZONTAL_FLIP): cv.boolean,
            cv.Optional(CONF_VERTICAL_FLIP): cv.boolean,
            cv.GenerateID(CONF_I2C_ID): cv.use_id(i2c.InternalI2CBus),
            cv.Required(CONF_SCCB_FREQUENCY): cv.All(
                cv.frequency, cv.Range(min=10000, max=1000000)
            ),
            cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_POWER_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_EXTERNAL_CLOCK): cv.Schema(
                {
                    cv.Required(CONF_PIN): pins.internal_gpio_output_pin_number,
                    cv.Optional(CONF_FREQUENCY, default="24MHz"): cv.All(
                        cv.frequency, cv.Range(min=6e6, max=40e6)
                    ),
                }
            ),
            cv.Optional(CONF_ON_STREAM_START): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        MipiCsiStreamStartTrigger
                    )
                }
            ),
            cv.Optional(CONF_ON_STREAM_STOP): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        MipiCsiStreamStopTrigger
                    )
                }
            ),
            cv.Optional(CONF_ON_IMAGE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MipiCsiImageTrigger)}
            ),
            # set_preview_buffer(): each frame copied into it; the buffer handed back when the preview ends
            cv.Optional(CONF_ON_PREVIEW_FRAME): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MipiCsiPreviewFrameTrigger)}
            ),
            cv.Optional(CONF_ON_PREVIEW_RELEASED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(MipiCsiPreviewReleasedTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_framework(Framework.ESP_IDF),
    esp32.only_on_variant(supported=[VARIANT_ESP32P4]),
    _validate_h264_format,
)

SETTERS = {
    CONF_PIXEL_FORMAT: "set_pixel_format",
    CONF_JPEG_QUALITY: "set_jpeg_quality",
    CONF_HORIZONTAL_FLIP: "set_horizontal_flip",
    CONF_VERTICAL_FLIP: "set_vertical_flip",
    CONF_FRAME_BUFFER_COUNT: "set_frame_buffer_count",
    CONF_RESET_PIN: "set_reset_pin",
    CONF_POWER_PIN: "set_power_pin",
}


async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_CAMERA")
    var = cg.new_Pvariable(config[CONF_ID])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)

    for key, setter in SETTERS.items():
        if key in config:
            cg.add(getattr(var, setter)(config[key]))

    if (resolution := config.get(CONF_RESOLUTION)) is not None:
        cg.add(var.set_resolution(*resolution))
    cg.add(var.set_sensor_name(config[CONF_SENSOR]))
    cg.add(var.set_init_ldo(config.get(CONF_INIT_LDO)))
    cg.add(var.set_framerate(int(config[CONF_FRAMERATE])))
    cg.add(var.set_sccb_frequency(int(config[CONF_SCCB_FREQUENCY])))
    if (h264 := config.get(CONF_H264_ENCODER)) is not None:
        cg.add(var.set_h264_encoder(await cg.get_variable(h264)))

    i2c_bus = await cg.get_variable(config[CONF_I2C_ID])
    cg.add(var.set_i2c_bus(i2c_bus))

    if (external_clock := config.get(CONF_EXTERNAL_CLOCK)) is not None:
        cg.add(
            var.set_external_clock(
                external_clock[CONF_PIN], int(external_clock[CONF_FREQUENCY])
            )
        )
        add_idf_sdkconfig_option("CONFIG_CAMERA_XCLK_USE_ESP_CLOCK_ROUTER", True)

    for conf_key, args in (
        (CONF_ON_STREAM_START, []),
        (CONF_ON_STREAM_STOP, []),
        (CONF_ON_IMAGE, [(CameraImageData, "image")]),
        (CONF_ON_PREVIEW_FRAME, []),
        (CONF_ON_PREVIEW_RELEASED, [(cg.uint8.operator("ptr"), "buffer")]),
    ):
        for conf in config.get(conf_key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, args, conf)

    _add_idf_config(config)


_ESP_VIDEO_OVERRIDE_MARKER = ".esphome_mipi_csi_patch"


def _sync_esp_video_override() -> str:
    """esp_video downloaded, its UVC device removed (idf_video_patch.py), in the build's components-src."""
    import io
    import shutil
    import zipfile

    from esphome.external_files import compute_local_file_path, download_content

    from . import idf_video_patch as patch

    dest = Path(CORE.build_path) / "components-src" / "espressif__esp_video"
    marker = dest / _ESP_VIDEO_OVERRIDE_MARKER
    if marker.is_file() and marker.read_text() == patch.PATCH_STAMP:
        return str(dest)

    archive = download_content(
        patch.ESP_VIDEO_URL,
        compute_local_file_path("mipi_csi", patch.ESP_VIDEO_URL),
    )
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        zf.extractall(dest)

    for rel in patch.REMOVED:
        target = dest / rel
        if not target.exists():
            raise cv.Invalid(
                f"mipi_csi: {rel} is missing from esp_video {patch.ESP_VIDEO_VERSION}; "
                f"the UVC removal does not fit this version"
            )
        if target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink()
    for rel, old, new in patch.PATCHES:
        target = dest / rel
        text = target.read_text(encoding="utf-8") if target.is_file() else ""
        if text.count(old) != 1:
            raise cv.Invalid(
                f"mipi_csi: the UVC removal no longer applies to {rel} in esp_video "
                f"{patch.ESP_VIDEO_VERSION} -- the code it anchors on is missing or appears more than once"
            )
        target.write_text(text.replace(old, new, 1), encoding="utf-8")

    marker.write_text(patch.PATCH_STAMP)
    return str(dest)


ESP_IPA_COMMIT = "5ec4d12101d7308dd9f826ff211312387f63e22d"
ESP_IPA_URL = f"https://codeload.github.com/espressif/esp-video-components/zip/{ESP_IPA_COMMIT}"


def _sync_esp_ipa_override() -> str:
    """esp_ipa of esp-video-components ESP_IPA_COMMIT (with its ESP32-P4 rev < 3 library) in components-src."""
    import io
    import shutil
    import zipfile

    from esphome.external_files import compute_local_file_path, download_content

    dest = Path(CORE.build_path) / "components-src" / "espressif__esp_ipa"
    marker = dest / _ESP_VIDEO_OVERRIDE_MARKER
    if marker.is_file() and marker.read_text() == ESP_IPA_COMMIT:
        return str(dest)

    archive = download_content(ESP_IPA_URL, compute_local_file_path("mipi_csi", ESP_IPA_URL))
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    prefix = f"esp-video-components-{ESP_IPA_COMMIT}/esp_ipa/"
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        for info in zf.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            if info.filename[len(prefix) :].startswith("test_apps/"):
                continue  # their own manifests, not the component
            target = dest / info.filename[len(prefix) :]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(zf.read(info))
    if not (dest / "lib" / "esp32p4" / "v6.0+" / "libesp_ipa_p4_less_v3.a").is_file():
        raise cv.Invalid(f"mipi_csi: esp_ipa {ESP_IPA_COMMIT} has no libesp_ipa_p4_less_v3.a")

    marker.write_text(ESP_IPA_COMMIT)
    return str(dest)


ESP_CAM_SENSOR_VERSION = "2.6.0"
ESP_CAM_SENSOR_URL = (
    "https://components-file.espressif.com/components/espressif/esp_cam_sensor/"
    f"{ESP_CAM_SENSOR_VERSION}/espressif__esp_cam_sensor-v{ESP_CAM_SENSOR_VERSION}.zip"
)
OV02C10_PATCHES = (
    (
        "CMakeLists.txt",
        "idf_component_register(SRCS ${srcs}\n",
        "if(CONFIG_CAMERA_OV02C10)\n"
        '    list(APPEND srcs "sensors/ov02c10/ov02c10.c")\n'
        '    list(APPEND include_dirs "sensors/ov02c10/include")\n'
        '    list(APPEND priv_include_dirs "sensors/ov02c10/private_include")\n'
        "endif()\n\n"
        "idf_component_register(SRCS ${srcs}\n",
    ),
    (
        "CMakeLists.txt",
        "if(CONFIG_CAMERA_SENSOR_MOTOR_DETECT_METHOD_DYNAMIC_LINK)\n"
        "    if(CONFIG_CAMERA_OV2640_AUTO_DETECT_DVP_INTERFACE_SENSOR)\n",
        "if(CONFIG_CAMERA_SENSOR_MOTOR_DETECT_METHOD_DYNAMIC_LINK)\n"
        "    if(CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR)\n"
        '        target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov02c10_detect")\n'
        "    endif()\n\n"
        "    if(CONFIG_CAMERA_OV2640_AUTO_DETECT_DVP_INTERFACE_SENSOR)\n",
    ),
    (
        "Kconfig",
        '            rsource "sensors/gc2145/Kconfig.gc2145"\n',
        '            rsource "sensors/gc2145/Kconfig.gc2145"\n'
        '            rsource "sensors/ov02c10/Kconfig.ov02c10"\n',
    ),
    (
        "project_include.cmake",
        "if(CONFIG_CAMERA_OV5647)\n",
        "if(CONFIG_CAMERA_OV02C10)\n"
        "    if(CONFIG_CAMERA_OV02C10_DEFAULT_IPA_JSON_CONFIGURATION_FILE)\n"
        '        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${COMPONENT_PATH}/sensors/ov02c10/cfg/'
        'ov02c10_default.json" APPEND)\n'
        "    elseif(CONFIG_CAMERA_OV02C10_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE)\n"
        "        idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "
        "${CONFIG_CAMERA_OV02C10_CUSTOMIZED_IPA_JSON_CONFIGURATION_FILE_PATH} APPEND)\n"
        "    endif()\n"
        "endif()\n\n"
        "if(CONFIG_CAMERA_OV5647)\n",
    ),
)


def _sync_esp_cam_sensor_override() -> str:
    import hashlib
    import io
    import shutil
    import zipfile

    from esphome.external_files import compute_local_file_path, download_content

    driver = Path(__file__).parent / "ov02c10"
    digest = hashlib.sha256(repr((ESP_CAM_SENSOR_URL, OV02C10_PATCHES)).encode())
    for path in sorted(p for p in driver.rglob("*") if p.is_file()):
        digest.update(path.relative_to(driver).as_posix().encode())
        digest.update(path.read_bytes())
    stamp = digest.hexdigest()
    dest = Path(CORE.build_path) / "components-src" / "espressif__esp_cam_sensor"
    marker = dest / _ESP_VIDEO_OVERRIDE_MARKER
    if marker.is_file() and marker.read_text() == stamp:
        return str(dest)

    archive = download_content(ESP_CAM_SENSOR_URL, compute_local_file_path("mipi_csi", ESP_CAM_SENSOR_URL))
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        zf.extractall(dest)
    shutil.copytree(driver, dest / "sensors" / "ov02c10")
    for rel, old, new in OV02C10_PATCHES:
        target = dest / rel
        text = target.read_text(encoding="utf-8") if target.is_file() else ""
        if text.count(old) != 1:
            raise cv.Invalid(
                f"mipi_csi: the OV02C10 addition no longer applies to {rel} in esp_cam_sensor "
                f"{ESP_CAM_SENSOR_VERSION} -- the code it anchors on is missing or appears more than once"
            )
        target.write_text(text.replace(old, new, 1), encoding="utf-8")

    marker.write_text(stamp)
    return str(dest)


def _add_idf_config(config: ConfigType) -> None:
    # A copy without its USB UVC device (idf_video_patch.py): USB cameras are usb_uvc's
    from . import idf_video_patch as patch

    add_idf_component(
        name="espressif/esp_video",
        ref=patch.ESP_VIDEO_VERSION,
        override_path=_sync_esp_video_override(),
    )
    if config[CONF_SENSOR] == "OV02C10":
        add_idf_component(
            name="espressif/esp_cam_sensor",
            ref=ESP_CAM_SENSOR_VERSION,
            override_path=_sync_esp_cam_sensor_override(),
        )
    else:
        add_idf_component(name="espressif/esp_cam_sensor", ref=ESP_CAM_SENSOR_VERSION)
    # esp_ipa 2.4.0's IDF 6 library is built for ESP32-P4 rev >= 3 (Zba / Zbb instructions): on a rev < 3 chip
    # the ISP pipeline's create_ipa_array() trapped (illegal instruction, sh2add). The commit that adds
    # libesp_ipa_p4_less_v3*.a (picked with CONFIG_ESP32P4_SELECTS_REV_LESS_V3), still version 2.4.0. As an
    # override: a git source beside esp_video's registry requirement was resolved from the registry anyway
    add_idf_component(
        name="espressif/esp_ipa",
        ref="2.4.0",
        override_path=_sync_esp_ipa_override(),
    )

    sensor = config[CONF_SENSOR]
    if (formats := SENSOR_FORMATS.get(sensor)) is not None:
        add_idf_sdkconfig_option(f"CONFIG_CAMERA_{sensor}", True)
        for format_option in formats:
            add_idf_sdkconfig_option(f"CONFIG_{format_option}", True)

    for option, value in {
        # The MIPI-CSI capture path is the only one used. DVP defaults to on, so turn it off; SPI
        # is already off unless an SPI sensor selects it, so it needs no entry here.
        "CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE": True,
        "CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE": False,
        # The ISP debayers RAW sensors. Its pipeline controller is what runs the IPA algorithms
        # (auto exposure, auto white balance, denoise) and is off by default.
        "CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE": True,
        "CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER": True,
        # The driver's own backup buffer would be allocated outside the buffer allocator: off, and one of the
        # frame buffers is its reserve instead (frame_buffer_count counts it).
        "CONFIG_ESP_VIDEO_DISABLE_MIPI_CSI_DRIVER_BACKUP_BUFFER": True,
        # The video devices are reached as /dev/videoN, so open(), ioctl() and mmap() all go
        # through VFS. Select support is deliberately not requested: the capture task blocks in
        # VIDIOC_DQBUF and esp_video registers no select handlers, so ESPHome is free to leave
        # CONFIG_VFS_SUPPORT_SELECT off and save the code it would otherwise pull in.
        "CONFIG_VFS_SUPPORT_IO": True,
    }.items():
        add_idf_sdkconfig_option(option, value)
