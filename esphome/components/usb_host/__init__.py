import esphome.codegen as cg
from esphome.components import esp32, socket
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_sdkconfig_option,
    get_esp32_variant,
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
        # Dual host mode: instances required, no top-level id
        if CONF_INSTANCES not in config:
            raise cv.Invalid("'instances' required when dual_host_support is enabled")
    elif CONF_INSTANCES in config:
        # Single host mode: instances not allowed
        raise cv.Invalid("'instances' only allowed when dual_host_support is enabled")

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
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
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_schema,
    cv.only_with_esp_idf,
    only_on_variant(supported=[VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32P4]),
)


async def register_usb_client(config, parent):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
    cg.add(
        var.set_parent(parent)
    )  # NEW: Set USBHost as parent for claiming coordination
    return var


async def to_code(config: ConfigType) -> None:
    dual_host_support = config.get(CONF_DUAL_HOST_SUPPORT)

    # Load modified esp-usb USB Host library for dual host support
    # TinyUSB is now included directly in esp-usb repo (not as submodule)
    # Use override_path to replace the built-in ESP-IDF "usb" component
    if dual_host_support:
        esp32.add_idf_component(
            name="usb",
            repo="https://github.com/p1ngb4ck/esp-usb.git",
            ref="feat/dual-host-support",
            path="host/usb",
            override_path="host/usb",
        )
        # Add custom TinyUSB library with dual-instance support
        # This provides the TinyUSB headers and implementation
        cg.add_library(
            name="TinyUSB",
            repository="https://github.com/p1ngb4ck/tinyusb.git",
            version="feat/dual-host-support",
        )

    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
    if config.get(CONF_ENABLE_HUBS):
        add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", True)

    max_requests = config[CONF_MAX_TRANSFER_REQUESTS]
    cg.add_define("USB_HOST_MAX_REQUESTS", max_requests)

    if dual_host_support:
        cg.add_define("USE_USB_HOST_DUAL_INSTANCE")

    # USB uses the socket wake_loop_threadsafe() mechanism to wake the main loop from USB task
    # This enables low-latency (~12μs) USB event processing instead of waiting for
    # select() timeout (0-16ms). The wake socket is shared across all components.
    socket.require_wake_loop_threadsafe()

    from esphome.core import CORE

    if not hasattr(CORE, "data"):
        CORE.data = {}
    CORE.data["usb_host_dual_instance"] = dual_host_support

    if dual_host_support and CONF_INSTANCES in config:
        # Dual host mode: create multiple USBHost instances
        usb_host_instances = {}
        for instance_conf in config[CONF_INSTANCES]:
            var = cg.new_Pvariable(instance_conf[CONF_ID])
            await cg.register_component(var, instance_conf)
            controller_type = instance_conf[CONF_CONTROLLER]
            # Set the controller type (0 = FS, 1 = HS) for TinyUSB initialization
            cg.add(var.set_controller_type(controller_type))

            # Register devices as USBClient components and add to whitelist
            for device in instance_conf.get(CONF_DEVICES) or ():
                await register_usb_client(device, var)
                cg.add(var.add_device_to_whitelist(device[CONF_VID], device[CONF_PID]))

            usb_host_instances[instance_conf[CONF_ID]] = {
                "var": var,
                "controller": controller_type,
            }
        CORE.data["usb_host_instances"] = usb_host_instances
    else:
        # Single host mode (default): create one USBHost instance
        var = cg.new_Pvariable(config[CONF_ID])
        await cg.register_component(var, config)
        CORE.data["usb_host_instance"] = var

    # Register devices as USBClient components and add to whitelist
    # In dual host mode, devices are specified per-instance (handled above)
    # In single host mode, devices are specified at top level
    if not dual_host_support:
        for device in config.get(CONF_DEVICES) or ():
            await register_usb_client(device, CORE.data["usb_host_instance"])
            cg.add(
                CORE.data["usb_host_instance"].add_device_to_whitelist(
                    device[CONF_VID], device[CONF_PID]
                )
            )
