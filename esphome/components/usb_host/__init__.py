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
from esphome.helpers import write_file_if_changed
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


def _patch_usb_host_dual_phy() -> None:
    """Patch IDF usb_host.c to support dual-PHY init on ESP32-P4.

    IDF 5.5.x initialises only one PHY regardless of peripheral_map bits.
    Replace the single-PHY block with a loop over all enabled bits so that
    both HS and FS controllers are initialised when dual_host is enabled.
    Idempotent: already-patched files are a no-op.
    """
    idf_path = _get_idf_path()
    if idf_path is None:
        return
    usb_host_c = idf_path / "components" / "usb" / "usb_host.c"
    if not usb_host_c.is_file():
        return

    try:
        content = usb_host_c.read_text(encoding="utf-8")
    except OSError as e:
        _LOGGER.warning("Could not read %s for dual-PHY patch: %s", usb_host_c, e)
        return

    if "PHY install error for port" in content:
        return  # already patched

    old_struct = "        usb_phy_handle_t phy_handle;    // Will be NULL if host library is installed with skip_phy_setup"
    new_struct = "        usb_phy_handle_t phy_handles[SOC_USB_OTG_PERIPH_NUM];  // patched: one per port"

    old_init = (
        "    // Install USB PHY (if necessary). USB PHY driver will also enable the underlying Host Controller\n"
        "    if (!config->skip_phy_setup) {\n"
        "        bool init_utmi_phy = false; // Default value for Linux simulation\n"
        "\n"
        "#if SOC_USB_OTG_SUPPORTED // In case we run on a real target, select the PHY from usb_dwc_info description structure\n"
        "        // Right now we support only one peripheral, can be extended in future\n"
        "        int peripheral_index = 0;\n"
        "        if (peripheral_map & BIT1) {\n"
        "            peripheral_index = 1;\n"
        "        }\n"
        "        init_utmi_phy = (usb_dwc_info.controllers[peripheral_index].supported_phys == USB_PHY_INST_UTMI_0);\n"
        "#endif // SOC_USB_OTG_SUPPORTED\n"
        "\n"
        "        // Host Library defaults to internal PHY\n"
        "        usb_phy_config_t phy_config = {\n"
        "            .controller = USB_PHY_CTRL_OTG,\n"
        "            .target = init_utmi_phy ? USB_PHY_TARGET_UTMI : USB_PHY_TARGET_INT,\n"
        "            .otg_mode = USB_OTG_MODE_HOST,\n"
        "            .otg_speed = USB_PHY_SPEED_UNDEFINED,   // In Host mode, the speed is determined by the connected device\n"
        "            .ext_io_conf = NULL,\n"
        "            .otg_io_conf = NULL,\n"
        "        };\n"
        "        ret = usb_new_phy(&phy_config, &host_lib_obj->constant.phy_handle);\n"
        "        if (ret != ESP_OK) {\n"
        '            ESP_LOGE(USB_HOST_TAG, "PHY install error: %s", esp_err_to_name(ret));\n'
        "            goto phy_err;\n"
        "        }\n"
        "    }"
    )
    new_init = (
        "    // Install USB PHY for each enabled peripheral (patched by ESPHome for dual-host on ESP32-P4)\n"
        "    if (!config->skip_phy_setup) {\n"
        "        memset(host_lib_obj->constant.phy_handles, 0, sizeof(host_lib_obj->constant.phy_handles));\n"
        "        for (int i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {\n"
        "            if (!(peripheral_map & BIT(i))) {\n"
        "                continue;\n"
        "            }\n"
        "            bool init_utmi_phy = false;\n"
        "#if SOC_USB_OTG_SUPPORTED\n"
        "            init_utmi_phy = (usb_dwc_info.controllers[i].supported_phys == USB_PHY_INST_UTMI_0);\n"
        "#endif\n"
        "            usb_phy_config_t phy_config = {\n"
        "                .controller = USB_PHY_CTRL_OTG,\n"
        "                .target = init_utmi_phy ? USB_PHY_TARGET_UTMI : USB_PHY_TARGET_INT,\n"
        "                .otg_mode = USB_OTG_MODE_HOST,\n"
        "                .otg_speed = USB_PHY_SPEED_UNDEFINED,\n"
        "                .ext_io_conf = NULL,\n"
        "                .otg_io_conf = NULL,\n"
        "            };\n"
        "            ret = usb_new_phy(&phy_config, &host_lib_obj->constant.phy_handles[i]);\n"
        "            if (ret != ESP_OK) {\n"
        '                ESP_LOGE(USB_HOST_TAG, "PHY install error for port %d: %s", i, esp_err_to_name(ret));\n'
        "                goto phy_err;\n"
        "            }\n"
        "        }\n"
        "    }"
    )

    old_del_install = (
        "    if (host_lib_obj->constant.phy_handle) {\n"
        "        ESP_ERROR_CHECK(usb_del_phy(host_lib_obj->constant.phy_handle));\n"
        "    }\n"
        "phy_err:"
    )
    new_del_install = (
        "    for (int i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {\n"
        "        if (host_lib_obj->constant.phy_handles[i]) {\n"
        "            ESP_ERROR_CHECK(usb_del_phy(host_lib_obj->constant.phy_handles[i]));\n"
        "        }\n"
        "    }\n"
        "phy_err:"
    )

    old_del_uninstall = (
        "    // If the USB PHY was setup, then delete it\n"
        "    if (host_lib_obj->constant.phy_handle) {\n"
        "        ESP_ERROR_CHECK(usb_del_phy(host_lib_obj->constant.phy_handle));\n"
        "    }"
    )
    new_del_uninstall = (
        "    // If the USB PHY was setup, then delete it\n"
        "    for (int i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {\n"
        "        if (host_lib_obj->constant.phy_handles[i]) {\n"
        "            ESP_ERROR_CHECK(usb_del_phy(host_lib_obj->constant.phy_handles[i]));\n"
        "        }\n"
        "    }"
    )

    if old_struct not in content or old_init not in content:
        _LOGGER.warning("usb_host.c: expected patterns not found, skipping dual-PHY patch.")
        return

    content = content.replace(old_struct, new_struct)
    content = content.replace(old_init, new_init)
    content = content.replace(old_del_install, new_del_install)
    content = content.replace(old_del_uninstall, new_del_uninstall)
    write_file_if_changed(usb_host_c, content)
    _LOGGER.info("Patched %s for dual-PHY init (ESP32-P4 dual USB host).", usb_host_c)


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
