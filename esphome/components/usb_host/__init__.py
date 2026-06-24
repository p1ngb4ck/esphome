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
from esphome.espidf.toolchain import _get_idf_path
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
_ESP_USB_REF = "usb-v1.4.1"
_ESP_USB_SRCS = [
    "enum.c",
    "ext_hub.c",
    "ext_port.c",
    "hcd_dwc.c",
    "hub.c",
    "usbh.c",
    "usb_helpers.c",
    "usb_host.c",
    "usb_private.c",
]
_ESP_USB_PRIVATE_HDRS = [
    "enum.h",
    "ext_hub.h",
    "ext_port.h",
    "hcd.h",
    "hub.h",
    "usbh.h",
    "usb_private.h",
]


def _patch_usb_host_dual_phy() -> None:
    """Replace IDF 5.5.x USB host sources with espressif/usb 1.4.1 for dual-host on ESP32-P4.

    IDF 5.5.x usb_host.c, hub.c, and hcd_dwc.c only support a single root port.
    espressif/usb 1.4.1 adds full dual-port support (HCD_NUM_PORTS loop in hub,
    PHY init loop in usb_host, per-port HCD in hcd_dwc).
    Idempotent: skips if already patched (sentinel string present).
    """
    import shutil
    import subprocess
    import tempfile
    from pathlib import Path as _Path

    idf_path = _get_idf_path()
    if idf_path is None:
        return
    usb_dir = _Path(idf_path) / "components" / "usb"
    if not usb_dir.is_dir():
        return

    sentinel = "root_hub_ports[HCD_NUM_PORTS]"
    if sentinel in (usb_dir / "hub.c").read_text(encoding="utf-8"):
        return  # already patched

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = _Path(tmp)
        _LOGGER.info("Cloning espressif/usb %s for dual-host patch...", _ESP_USB_REF)
        result = subprocess.run(
            [
                "git", "clone", "--depth=1", "--branch", _ESP_USB_REF,
                _ESP_USB_REPO, str(tmp_path / "esp-usb"),
            ],
            capture_output=True,
        )
        if result.returncode != 0:
            _LOGGER.warning(
                "Failed to clone espressif/usb: %s", result.stderr.decode()
            )
            return

        src_root = tmp_path / "esp-usb" / "host" / "usb"

        for fname in _ESP_USB_SRCS:
            src = src_root / "src" / fname
            dst = usb_dir / fname
            if src.is_file():
                shutil.copy2(src, dst)
            else:
                _LOGGER.warning("espressif/usb: source file not found: %s", src)

        priv_inc_dst = usb_dir / "private_include"
        priv_inc_dst.mkdir(exist_ok=True)
        for fname in _ESP_USB_PRIVATE_HDRS:
            src = src_root / "private_include" / fname
            dst = priv_inc_dst / fname
            if src.is_file():
                shutil.copy2(src, dst)
            else:
                _LOGGER.warning("espressif/usb: private header not found: %s", src)

    _LOGGER.info(
        "Patched IDF USB host sources with espressif/usb %s for dual-host on ESP32-P4.",
        _ESP_USB_REF,
    )


async def to_code(config: ConfigType) -> None:
    # espressif/usb 1.4.1 supports IDF >= 5.5.3 and adds dual-host on P4.
    # Use it unconditionally — on IDF 5.x it overrides the built-in usb component
    # (pulling usb_phy.c from the overridden dir automatically via its CMakeLists).
    # On IDF >= 6.0 it is the primary component anyway.
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
        _patch_usb_host_dual_phy()

    for device in config.get(CONF_DEVICES) or ():
        await register_usb_client(device)
