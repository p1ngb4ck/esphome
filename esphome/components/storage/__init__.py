from __future__ import annotations

import logging

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = []
AUTO_LOAD = []

# Namespaces
storage_ns = cg.esphome_ns.namespace("storage")

# Classes
Storage = storage_ns.class_("Storage", cg.Component)
StorageMount = storage_ns.class_("StorageMount")
FileManager = storage_ns.class_("FileManager", cg.Component)

# Triggers - FileManager triggers
FileAddedTrigger = storage_ns.class_("FileAddedTrigger", automation.Trigger)
FileModifiedTrigger = storage_ns.class_("FileModifiedTrigger", automation.Trigger)
FileDeletedTrigger = storage_ns.class_("FileDeletedTrigger", automation.Trigger)
DirectoryChangedTrigger = storage_ns.class_(
    "DirectoryChangedTrigger", automation.Trigger
)

# Triggers - StorageDevice registry triggers
DeviceAddedTrigger = storage_ns.class_("DeviceAddedTrigger", automation.Trigger)
DeviceRemovedTrigger = storage_ns.class_("DeviceRemovedTrigger", automation.Trigger)
DeviceChangedTrigger = storage_ns.class_("DeviceChangedTrigger", automation.Trigger)

# StorageDevice pointer type for trigger parameters
StorageDevice = storage_ns.class_("StorageDevice")

# Info structs
FileInfo = storage_ns.struct("FileInfo")
DirectoryChangeInfo = storage_ns.struct("DirectoryChangeInfo")

# Configuration keys
CONF_MOUNTS = "mounts"
CONF_MOUNT_PATH = "path"
CONF_MOUNT_PLATFORM = "platform"
CONF_FILE_MANAGERS = "file_managers"
CONF_STORAGE_ID = "storage_id"
CONF_WATCH_DIRECTORY = "watch_directory"
CONF_WATCH_FILE = "watch_file"
CONF_SCAN_INTERVAL = "scan_interval"
CONF_PATTERNS = "patterns"
CONF_MIN_SIZE = "min_size"
CONF_MAX_SIZE = "max_size"
CONF_ON_FILE_ADDED = "on_file_added"
CONF_ON_FILE_MODIFIED = "on_file_modified"
CONF_ON_FILE_DELETED = "on_file_deleted"
CONF_ON_DIRECTORY_CHANGED = "on_directory_changed"
CONF_ON_DEVICE_ADDED = "on_device_added"
CONF_ON_DEVICE_REMOVED = "on_device_removed"
CONF_ON_DEVICE_CHANGED = "on_device_changed"

# Platform types
PLATFORM_SD_DIRECT = "sd_direct"
PLATFORM_USB_MSC = "usb_msc"
PLATFORM_SD_MMC = "sd_mmc"
PLATFORM_SPIFFS = "spiffs"
PLATFORM_NFS = "nfs"
PLATFORM_SMB = "smb"
PLATFORM_BINARY_STORAGE = "binary_storage"

# Single mount configuration
MOUNT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(StorageMount),
        cv.Required(CONF_MOUNT_PATH): cv.string,
        cv.Optional(CONF_MOUNT_PLATFORM, default=PLATFORM_SD_DIRECT): cv.one_of(
            PLATFORM_SD_DIRECT,
            PLATFORM_USB_MSC,
            PLATFORM_SD_MMC,
            PLATFORM_SPIFFS,
            PLATFORM_NFS,
            PLATFORM_SMB,
            PLATFORM_BINARY_STORAGE,
        ),
    }
)

# File manager configuration with automation triggers
FILE_MANAGER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FileManager),
        cv.GenerateID(CONF_STORAGE_ID): cv.use_id(Storage),
        # Watch either a directory or a single file (mutually exclusive)
        cv.Optional(CONF_WATCH_DIRECTORY): cv.string,
        cv.Optional(CONF_WATCH_FILE): cv.string,
        # Scan interval
        cv.Optional(
            CONF_SCAN_INTERVAL, default="5s"
        ): cv.positive_time_period_milliseconds,
        # Pattern filters (glob patterns like "*.jpg", "*.png")
        cv.Optional(CONF_PATTERNS, default=[]): cv.ensure_list(cv.string),
        # Size filters
        cv.Optional(CONF_MIN_SIZE, default=0): cv.int_range(min=0),
        cv.Optional(CONF_MAX_SIZE): cv.int_range(min=0),
        # Automation triggers
        cv.Optional(CONF_ON_FILE_ADDED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FileAddedTrigger),
            }
        ),
        cv.Optional(CONF_ON_FILE_MODIFIED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FileModifiedTrigger),
            }
        ),
        cv.Optional(CONF_ON_FILE_DELETED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FileDeletedTrigger),
            }
        ),
        cv.Optional(CONF_ON_DIRECTORY_CHANGED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DirectoryChangedTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

# Storage configuration
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Storage),
        cv.Optional(CONF_MOUNTS, default=[]): cv.ensure_list(MOUNT_SCHEMA),
        cv.Optional(CONF_FILE_MANAGERS, default=[]): cv.ensure_list(
            FILE_MANAGER_SCHEMA
        ),
        # Device registry automation triggers
        cv.Optional(CONF_ON_DEVICE_ADDED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DeviceAddedTrigger),
            }
        ),
        cv.Optional(CONF_ON_DEVICE_REMOVED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DeviceRemovedTrigger),
            }
        ),
        cv.Optional(CONF_ON_DEVICE_CHANGED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DeviceChangedTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    """Generate code for storage component"""
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_define("USE_STORAGE")

    # Enable PSRAM buffer pool on ESP32 with guaranteed PSRAM
    from esphome.core import CORE

    if CORE.is_esp32:
        from esphome.components import psram

        if psram.is_guaranteed():
            cg.add_define("USE_PSRAM")

    # Register each mount
    for mount_config in config.get(CONF_MOUNTS, []):
        mount_path = mount_config[CONF_MOUNT_PATH]
        mount_platform = mount_config[CONF_MOUNT_PLATFORM]

        # Create StorageMount object for this mount
        mount_var = cg.new_Pvariable(mount_config[CONF_ID])
        cg.add(mount_var.set_path(mount_path))
        cg.add(mount_var.set_platform(mount_platform))
        cg.add(mount_var.set_storage(var))

        # Register with storage
        cg.add(var.register_mount(mount_path, mount_platform))

    # Register file managers
    if CONF_FILE_MANAGERS in config:
        for fm_config in config[CONF_FILE_MANAGERS]:
            await setup_file_manager(fm_config, var)

    # Register mount callbacks for SD storage devices
    # SD/USB devices store themselves in CORE.data during their to_code()
    # We register callbacks so when they mount, they register with storage
    from esphome.core import CORE

    if hasattr(CORE, "data") and "sd_storage_devices" in CORE.data:
        for sd_device in CORE.data["sd_storage_devices"]:
            # Register callback: when SD mounts, register it with storage
            cg.add(
                sd_device.add_mount_ready_callback(
                    cg.RawExpression(
                        f"[](const std::string &mount_path) {{ "
                        f"if (storage::global_storage != nullptr) {{ "
                        f"storage::global_storage->register_device({sd_device}); "
                        f"}} }}"
                    )
                )
            )

    if hasattr(CORE, "data") and "usb_storage_devices" in CORE.data:
        for usb_device in CORE.data["usb_storage_devices"]:
            # Register callback: when USB mounts, register it with storage
            cg.add(
                usb_device.add_mount_ready_callback(
                    cg.RawExpression(
                        f"[](const std::string &mount_path) {{ "
                        f"if (storage::global_storage != nullptr) {{ "
                        f"storage::global_storage->register_device({usb_device}); "
                        f"}} }}"
                    )
                )
            )

    # Device registry automation triggers
    for conf in config.get(CONF_ON_DEVICE_ADDED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(StorageDevice.operator("ptr"), "device")], conf
        )

    for conf in config.get(CONF_ON_DEVICE_REMOVED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(StorageDevice.operator("ptr"), "device")], conf
        )

    for conf in config.get(CONF_ON_DEVICE_CHANGED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(StorageDevice.operator("ptr"), "device")], conf
        )


async def setup_file_manager(config, parent_storage):
    """Configure a FileManager component with automation triggers"""
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Link to parent storage component
    storage = await cg.get_variable(config[CONF_STORAGE_ID])
    cg.add(var.set_storage(storage))

    # Set watch directory or file
    if CONF_WATCH_DIRECTORY in config:
        cg.add(var.set_watch_directory(config[CONF_WATCH_DIRECTORY]))
    elif CONF_WATCH_FILE in config:
        cg.add(var.set_watch_file(config[CONF_WATCH_FILE]))
    else:
        raise cv.Invalid("Either watch_directory or watch_file must be specified")

    # Scan interval
    cg.add(var.set_scan_interval(config[CONF_SCAN_INTERVAL]))

    # Pattern filters
    for pattern in config.get(CONF_PATTERNS, []):
        cg.add(var.add_pattern(pattern))

    # Size filters
    if CONF_MIN_SIZE in config:
        cg.add(var.set_min_size(config[CONF_MIN_SIZE]))
    if CONF_MAX_SIZE in config:
        cg.add(var.set_max_size(config[CONF_MAX_SIZE]))

    # Automation triggers
    for conf in config.get(CONF_ON_FILE_ADDED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(FileInfo, "x")], conf)
        cg.add(var.add_on_file_added_callback(trigger))

    for conf in config.get(CONF_ON_FILE_MODIFIED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(FileInfo, "x")], conf)
        cg.add(var.add_on_file_modified_callback(trigger))

    for conf in config.get(CONF_ON_FILE_DELETED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(FileInfo, "x")], conf)
        cg.add(var.add_on_file_deleted_callback(trigger))

    for conf in config.get(CONF_ON_DIRECTORY_CHANGED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(DirectoryChangeInfo, "x")], conf)
        cg.add(var.add_on_directory_changed_callback(trigger))

    return var
