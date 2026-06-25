import logging

import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_component,
    add_idf_sdkconfig_option,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_DEVICES, CONF_ID
from esphome.core import CORE
from esphome.cpp_types import Component
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

AUTO_LOAD = ["bytebuffer"]
CODEOWNERS = ["@clydebarrow"]
DEPENDENCIES = ["esp32"]
usb_host_ns = cg.esphome_ns.namespace("usb_host")
USBHost = usb_host_ns.class_("USBHost", Component)
USBClient = usb_host_ns.class_("USBClient", Component)
DOMAIN = "usb_host"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_ENABLE_HUBS = "enable_hubs"
CONF_MAX_TRANSFER_REQUESTS = "max_transfer_requests"
CONF_MAX_PACKET_SIZE = "max_packet_size"
CONF_DUAL_HOST = "dual_host"


def usb_device_schema(cls=USBClient, vid: int = None, pid: int = None) -> cv.Schema:
    schema = cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(cls),
        }
    )
    if vid:
        schema = schema.extend({cv.Optional(CONF_VID, default=vid): cv.hex_uint16_t})
    else:
        schema = schema.extend({cv.Required(CONF_VID): cv.hex_uint16_t})
    if pid:
        schema = schema.extend({cv.Optional(CONF_PID, default=pid): cv.hex_uint16_t})
    else:
        schema = schema.extend({cv.Required(CONF_PID): cv.hex_uint16_t})
    return schema


def _set_max_packet_size(config: dict) -> dict:
    CORE.data.setdefault(DOMAIN, {})[CONF_MAX_PACKET_SIZE] = config[
        CONF_MAX_PACKET_SIZE
    ]
    return config


def get_max_packet_size() -> int:
    return CORE.data.get(DOMAIN, {}).get(CONF_MAX_PACKET_SIZE, 64)


def _dual_host_validator(value):
    """dual_host is only valid (and meaningful) on ESP32-P4."""
    value = cv.boolean(value)
    if value:
        from esphome.components.esp32 import get_esp32_variant

        if get_esp32_variant() != VARIANT_ESP32P4:
            raise cv.Invalid("dual_host is only supported on ESP32-P4")
    return value


CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBHost),
            cv.Optional(CONF_ENABLE_HUBS, default=False): cv.boolean,
            cv.Optional(CONF_MAX_TRANSFER_REQUESTS, default=16): cv.int_range(
                min=1, max=32
            ),
            cv.Optional(CONF_MAX_PACKET_SIZE, default=64): cv.one_of(
                64, 128, 256, 512, 1024, int=True
            ),
            cv.Optional(CONF_DUAL_HOST, default=False): _dual_host_validator,
            cv.Optional(CONF_DEVICES): cv.ensure_list(usb_device_schema()),
        }
    ),
    only_on_variant(supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3]),
    _set_max_packet_size,
)


async def register_usb_client(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
    return var


_ESP_USB_REPO = "https://github.com/espressif/esp-usb.git"
_ESP_USB_REF = "master"


def _prepare_dual_host_usb_override() -> str | None:
    """Clone espressif/usb master into the build dir and return the local component path.

    The registry release (1.4.1) lacks dual-port hub support. By cloning master
    and registering the local copy as override_path, IDF uses those sources
    directly — no timing issues with managed_components download order.
    Returns the path to the local component root (host/usb/), or None on failure.
    """
    import subprocess
    from pathlib import Path as _Path

    override_dir = _Path(CORE.build_path) / "esphome_usb_override"
    clone_dir = override_dir / "esp-usb"
    component_dir = clone_dir / "host" / "usb"

    if component_dir.is_dir():
        _LOGGER.info("espressif/usb override already cloned at %s", component_dir)
        return str(component_dir)

    override_dir.mkdir(parents=True, exist_ok=True)
    _LOGGER.info("Cloning espressif/usb %s for dual-host override...", _ESP_USB_REF)
    result = subprocess.run(
        [
            "git", "clone", "--depth=1", "--branch", _ESP_USB_REF,
            _ESP_USB_REPO, str(clone_dir),
        ],
        capture_output=True,
    )
    if result.returncode != 0:
        _LOGGER.warning(
            "Failed to clone espressif/usb: %s", result.stderr.decode()
        )
        return None

    if not component_dir.is_dir():
        _LOGGER.warning(
            "espressif/usb cloned but host/usb/ not found at %s", component_dir
        )
        return None

    _LOGGER.info(
        "espressif/usb %s cloned to %s for dual-host on ESP32-P4.", _ESP_USB_REF, component_dir
    )
    return str(component_dir)


async def to_code(config: ConfigType) -> None:
    if config.get(CONF_DUAL_HOST):
        # Clone espressif/usb master (dual-port capable) into build dir and
        # use override_path so IDF compiles those sources directly — bypassing
        # the registry download which would be 1.4.1 (single-port hub).
        override_path = _prepare_dual_host_usb_override()
        if override_path:
            add_idf_component(name="espressif/usb", override_path=override_path)
        else:
            _LOGGER.warning("dual_host: override clone failed, falling back to registry 1.4.1")
            add_idf_component(name="espressif/usb", ref="1.4.1")
    else:
        add_idf_component(name="espressif/usb", ref="1.4.1")

    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
    if config.get(CONF_ENABLE_HUBS):
        add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", True)

    cg.add_define("USB_HOST_MAX_REQUESTS", config[CONF_MAX_TRANSFER_REQUESTS])
    cg.add_define("USB_HOST_MAX_PACKET_SIZE", config[CONF_MAX_PACKET_SIZE])

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if config.get(CONF_DUAL_HOST):
        cg.add(var.set_dual_host(True))

    for device in config.get(CONF_DEVICES) or ():
        await register_usb_client(device)
