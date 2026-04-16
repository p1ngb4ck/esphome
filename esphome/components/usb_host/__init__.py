import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_component,
    add_idf_sdkconfig_option,
    get_esp32_variant,
    idf_version,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_DEVICES, CONF_ID
from esphome.cpp_types import Component
from esphome.types import ConfigType

AUTO_LOAD = ["bytebuffer", "socket"]
CODEOWNERS = ["@clydebarrow"]
DEPENDENCIES = ["esp32"]
usb_host_ns = cg.esphome_ns.namespace("usb_host")
USBHost = usb_host_ns.class_("USBHost", Component)
USBClient = usb_host_ns.class_("USBClient", Component, cg.Parented.template(USBHost))

CONF_VID = "vid"
CONF_PID = "pid"
CONF_ENABLE_HUBS = "enable_hubs"
CONF_MAX_TRANSFER_REQUESTS = "max_transfer_requests"
CONF_DUAL_HOST_SUPPORT = "dual_host_support"
CONF_INSTANCES = "instances"
CONF_CONTROLLER = "controller"


def validate_dual_host_support(value):
    """Validate dual_host_support is only True on ESP32-P4."""
    value = cv.boolean(value)
    if value:
        variant = get_esp32_variant()
        if variant != VARIANT_ESP32P4:
            raise cv.Invalid(
                f"dual_host_support is only available on ESP32-P4, not {variant}"
            )
    return value


def usb_device_schema(
    cls=USBClient, vid: int = None, pid: list[int] = None
) -> cv.Schema:
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


USB_HOST_INSTANCE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(USBHost),
        cv.Required(CONF_CONTROLLER): cv.enum({"fs": 0, "hs": 1}, upper=False),
        cv.Optional(CONF_DEVICES): cv.ensure_list(usb_device_schema()),
    }
)


def _validate_schema(config):
    """Conditionally apply schema based on dual_host_support."""
    dual_host_support = config.get(CONF_DUAL_HOST_SUPPORT, False)

    if dual_host_support:
        # Dual host mode: instances required, no top-level devices
        if CONF_INSTANCES not in config:
            raise cv.Invalid("'instances' required when dual_host_support is enabled")
        if CONF_DEVICES in config:
            raise cv.Invalid(
                "'devices' not allowed at top level when dual_host_support is enabled. "
                "Place devices under instances."
            )
    elif CONF_INSTANCES in config:
        # Single host mode: instances not allowed
        raise cv.Invalid("'instances' only allowed when dual_host_support is enabled")

    return config


CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBHost),
            cv.Optional(CONF_ENABLE_HUBS, default=False): cv.boolean,
            cv.Optional(CONF_MAX_TRANSFER_REQUESTS, default=16): cv.int_range(
                min=1, max=32
            ),
            cv.Optional(
                CONF_DUAL_HOST_SUPPORT, default=False
            ): validate_dual_host_support,
            cv.Optional(CONF_INSTANCES): cv.ensure_list(USB_HOST_INSTANCE_SCHEMA),
            cv.Optional(CONF_DEVICES): cv.ensure_list(usb_device_schema()),
        }
    ),
    _validate_schema,
    only_on_variant(supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3]),
)


async def register_usb_client(config, parent=None):
    from esphome.core import CORE

    # Check if dual_host_support is enabled
    dual_host_support = CORE.data.get("usb_host_dual_instance", False)

    if dual_host_support and parent is not None:
        # Dual-host mode: pass parent as constructor parameter
        var = cg.new_Pvariable(
            config[CONF_ID], config[CONF_VID], config[CONF_PID], parent
        )
    else:
        # Singleton mode: do not pass parent parameter
        var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
    return var


async def to_code(config: ConfigType) -> None:
    dual_host_support = config.get(CONF_DUAL_HOST_SUPPORT)

    # Set TinyUSB MCU type based on ESP32 variant
    variant = get_esp32_variant()
    mcu_map = {
        VARIANT_ESP32S2: "OPT_MCU_ESP32S2",
        VARIANT_ESP32S3: "OPT_MCU_ESP32S3",
        VARIANT_ESP32P4: "OPT_MCU_ESP32P4",
    }
    if variant in mcu_map:
        cg.add_build_flag(f"-DCFG_TUSB_MCU={mcu_map[variant]}")

    # IDF 6.0 moved USB host to an external component
    if idf_version() >= cv.Version(6, 0, 0):
        add_idf_component(name="espressif/usb", ref="1.3.0")

    # Load modified esp-usb USB Host library for dual host support
    if dual_host_support:
        esp32.add_idf_component(
            name="usb",
            repo="https://github.com/p1ngb4ck/esp-usb.git",
            ref="feat/dual-host-support",
            path="host/usb",
            override_path="host/usb",
        )

    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
    if config.get(CONF_ENABLE_HUBS):
        add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", True)

    max_requests = config[CONF_MAX_TRANSFER_REQUESTS]
    cg.add_define("USB_HOST_MAX_REQUESTS", max_requests)

    if dual_host_support:
        cg.add_define("USE_USB_HOST_DUAL_INSTANCE")

    from esphome.core import CORE

    if not hasattr(CORE, "data"):
        CORE.data = {}
    CORE.data["usb_host_dual_instance"] = dual_host_support

    if dual_host_support and CONF_INSTANCES in config:
        # Dual host mode: create multiple USBHost instances
        usb_host_instances = {}

        # Calculate combined peripheral_map from user config values
        combined_peripheral_map = 0
        controller_map = {"fs": 0, "hs": 1}
        for instance_conf in config[CONF_INSTANCES]:
            controller_idx = controller_map[instance_conf[CONF_CONTROLLER]]
            # Invert mapping: controller 0 (FS) → OTG1 (BIT1), controller 1 (HS) → OTG0 (BIT0)
            peripheral_bit = 1 << (1 - controller_idx)
            combined_peripheral_map |= peripheral_bit

        for instance_conf in config[CONF_INSTANCES]:
            controller_index = controller_map[instance_conf[CONF_CONTROLLER]]
            var = cg.new_Pvariable(instance_conf[CONF_ID], controller_index)
            await cg.register_component(var, instance_conf)

            for device in instance_conf.get(CONF_DEVICES) or ():
                await register_usb_client(device, parent=var)
                cg.add(var.add_device_to_allowlist(device[CONF_VID], device[CONF_PID]))

            usb_host_instances[instance_conf[CONF_ID]] = {
                "var": var,
                "controller": controller_index,
            }

        cg.add_define(
            "USB_HOST_DUAL_PERIPHERAL_MAP", f"0x{combined_peripheral_map:02X}"
        )
        CORE.data["usb_host_instances"] = usb_host_instances
    else:
        # Single host mode (default): create one USBHost instance
        var = cg.new_Pvariable(config[CONF_ID])
        await cg.register_component(var, config)
        for device in config.get(CONF_DEVICES) or ():
            await register_usb_client(device)
            cg.add(var.add_device_to_allowlist(device[CONF_VID], device[CONF_PID]))
