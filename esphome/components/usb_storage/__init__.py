from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_component,
    include_builtin_idf_component,
    only_on_variant,
    require_fatfs,
    require_vfs_dir,
)
from esphome.components.usb_host import usb_host_ns
import esphome.config_validation as cv
from esphome.const import CONF_DEVICES, CONF_ID, CONF_TRIGGER_ID
from esphome.core import CORE

CODEOWNERS = ["p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]
AUTO_LOAD = []

CONF_MOUNT_PATH = "mount_path"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_ON_MOUNTED = "on_mounted"

require_vfs_dir()
require_fatfs()

usb_storage_ns = cg.esphome_ns.namespace("usb_storage")
USBStorageHost = usb_storage_ns.class_("USBStorageHost", cg.Component)
USBStorageDevice = usb_storage_ns.class_(
    "USBStorageDevice",
    usb_host_ns.class_("USBClient"),
    cg.Component,
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


async def register_usb_storage_handler(device_config, storage_host):
    var = cg.new_Pvariable(device_config[CONF_ID])
    await cg.register_component(var, device_config)
    cg.add(var.set_parent(storage_host))
    cg.add(var.set_mount_path(device_config[CONF_MOUNT_PATH]))
    cg.add(var.set_id(str(device_config[CONF_ID])))
    cg.add(var.set_vid(device_config[CONF_VID]))
    cg.add(var.set_pid(device_config[CONF_PID]))

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
            cv.Optional(CONF_DEVICES): cv.ensure_list(DEVICE_SCHEMA),
        }
    ),
    only_on_variant(supported=[VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32P4]),
)


async def to_code(config):
    include_builtin_idf_component("fatfs")
    add_idf_component(name="espressif/usb_host_msc", ref="1.1.4")

    # Create USBStorageHost
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    CORE.data.setdefault("usb_storage_devices", [])
    for device in config.get(CONF_DEVICES) or ():
        device_var = await register_usb_storage_handler(device, var)
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
