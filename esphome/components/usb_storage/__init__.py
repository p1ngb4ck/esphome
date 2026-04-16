from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_component,
    include_builtin_idf_component,
    only_on_variant,
    require_vfs_dir,
)
from esphome.components.usb_host import USBHost, usb_host_ns
import esphome.config_validation as cv
from esphome.const import CONF_DEVICES, CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]
AUTO_LOAD = []

CONF_USB_HOST_ID = "usb_host_id"
CONF_MOUNT_PATH = "mount_path"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_ON_MOUNTED = "on_mounted"

require_vfs_dir()

usb_storage_ns = cg.esphome_ns.namespace("usb_storage")
USBStorageHost = usb_storage_ns.class_("USBStorageHost", cg.Component)
USBStorageDevice = usb_storage_ns.class_(
    "USBStorageDevice",
    cg.Component,
    usb_host_ns.class_("USBDeviceHandler"),
    cg.Parented.template(USBStorageHost),
)

# Automation classes
DeviceMountedTrigger = usb_storage_ns.class_(
    "DeviceMountedTrigger", automation.Trigger.template(cg.std_string)
)
RemountDeviceAction = usb_storage_ns.class_("RemountDeviceAction", automation.Action)
UnmountDeviceAction = usb_storage_ns.class_("UnmountDeviceAction", automation.Action)
ListFilesAction = usb_storage_ns.class_("ListFilesAction", automation.Action)
DeviceMountedCondition = usb_storage_ns.class_(
    "DeviceMountedCondition", automation.Condition
)


async def register_usb_storage_handler(device_config, storage_host, usb_host):
    # Interface-class based handler with optional VID/PID filtering
    var = cg.new_Pvariable(device_config[CONF_ID])
    await cg.register_component(var, device_config)
    cg.add(var.set_parent(storage_host))  # Set USBStorageHost as parent
    cg.add(
        var.set_usb_host(usb_host)
    )  # Set USBHost reference for closing device handles

    # Set mount path
    cg.add(var.set_mount_path(device_config[CONF_MOUNT_PATH]))

    # Set ID for storage registry
    cg.add(var.set_id(str(device_config[CONF_ID])))

    # Set VID/PID (0x0000 means wildcard - match any)
    cg.add(var.set_vid(device_config[CONF_VID]))
    cg.add(var.set_pid(device_config[CONF_PID]))

    cg.add(
        usb_host.register_device_handler(var)
    )  # Register as interface-class handler with USBHost

    # Register on_mounted trigger
    for conf in device_config.get(CONF_ON_MOUNTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "mount_path")], conf
        )

    return var


DEVICE_SCHEMA = cv.COMPONENT_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(USBStorageDevice),
        cv.Required(CONF_MOUNT_PATH): cv.string,
        cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
        cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
        cv.Optional(CONF_ON_MOUNTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DeviceMountedTrigger),
            }
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBStorageHost),
            cv.GenerateID(CONF_USB_HOST_ID): cv.use_id(USBHost),
            cv.Optional(CONF_DEVICES): cv.ensure_list(DEVICE_SCHEMA),
        }
    ),
    only_on_variant(supported=[VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32P4]),
)


async def to_code(config):
    from esphome.core import CORE

    # Re-enable fatfs IDF component (excluded by default) - needed for esp_vfs_fat
    include_builtin_idf_component("fatfs")
    # Load appropriate MSC driver based on dual_host_support flag
    dual_host_support = CORE.data.get("usb_host_dual_instance", False)
    if dual_host_support:
        # Load modified multi-instance MSC driver from p1ngb4ck/esp-usb
        add_idf_component(
            name="usb_host_msc",
            repo="https://github.com/p1ngb4ck/esp-usb.git",
            ref="dual-host-support",
            path="host/class/msc/usb_host_msc",
        )
    else:
        # Load original singleton MSC driver
        add_idf_component(name="espressif/usb_host_msc", ref="1.1.4")

    # Get USBHost instance for handler registration
    usb_host_var = await cg.get_variable(config[CONF_USB_HOST_ID])

    # Create USBStorageHost
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # In dual host mode, pass USBHost reference for TinyUSB instance retrieval
    if dual_host_support:
        cg.add(var.set_usb_host(usb_host_var))

    # Register interface-class based handlers and store them for later use
    # by storage to register mount callbacks
    if not hasattr(CORE, "data"):
        CORE.data = {}
    if "usb_storage_devices" not in CORE.data:
        CORE.data["usb_storage_devices"] = []

    for device in config.get(CONF_DEVICES) or ():
        device_var = await register_usb_storage_handler(device, var, usb_host_var)
        # Store device reference in CORE.data for storage to access
        # This allows storage to register callbacks with USB storage devices
        CORE.data["usb_storage_devices"].append(device_var)


# Actions
USB_STORAGE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(USBStorageDevice),
    }
)


@automation.register_action(
    "usb_storage.remount",
    RemountDeviceAction,
    USB_STORAGE_ACTION_SCHEMA,
    synchronous=True,
)
async def usb_storage_remount_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "usb_storage.unmount",
    UnmountDeviceAction,
    USB_STORAGE_ACTION_SCHEMA,
    synchronous=True,
)
async def usb_storage_unmount_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "usb_storage.list_files",
    ListFilesAction,
    USB_STORAGE_ACTION_SCHEMA,
    synchronous=True,
)
async def usb_storage_list_files_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


# Conditions
@automation.register_condition(
    "usb_storage.is_mounted", DeviceMountedCondition, USB_STORAGE_ACTION_SCHEMA
)
async def usb_storage_is_mounted_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)
