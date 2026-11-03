from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32H4,
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32S31,
    add_idf_sdkconfig_option,
    get_esp32_variant,
    include_builtin_idf_component,
    only_on_variant,
    require_fatfs,
    require_vfs_dir,
    require_vfs_select,
)
from esphome.components.storage import (
    CONF_MOUNT_PATH,
    FILE_SYSTEM_SCHEMA_ENTRY,
    file_system_to_code,
    final_validate_file_system,
    register_mount_path,
    request_fatfs_path_length,
    request_storage_device,
    request_storage_worker,
    validate_file_system_value,
    validate_mount_path,
)
from esphome.components.usb_host import (
    USB_BLACKLIST_SCHEMA,
    note_usb_whitelist_used,
    setup_usb_match,
    usb_host_ns,
)
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import CONF_DEVICES, CONF_ID, CONF_TRIGGER_ID
from esphome.core import ID

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["storage", "usb_host"]
MULTI_CONF = True

CONF_VID = "vid"
CONF_PID = "pid"
CONF_ON_MOUNTED = "on_mounted"
CONF_FORMAT_ON_MISMATCH = "format_on_mismatch"
CONF_MAX_DEVICES = "max_devices"
# A stick without an entry: /usb, then /usb2 .. /usb9 while the path before is taken
DEFAULT_MOUNT_PATH = "/usb"

usb_storage_ns = cg.esphome_ns.namespace("usb_storage")

USBStorageClient = usb_storage_ns.class_(
    "USBStorageClient",
    usb_host_ns.class_("USBClient"),
    cg.Component,
)
USBStorageEntry = usb_storage_ns.struct("USBStorageEntry")

# Automation classes
DeviceMountedTrigger = usb_storage_ns.class_(
    "DeviceMountedTrigger", automation.Trigger.template(cg.const_char_ptr)
)
RemountDeviceAction = usb_storage_ns.class_("RemountDeviceAction", automation.Action)
UnmountDeviceAction = usb_storage_ns.class_("UnmountDeviceAction", automation.Action)
ListFilesAction = usb_storage_ns.class_("ListFilesAction", automation.Action)
DeviceMountedCondition = usb_storage_ns.class_(
    "DeviceMountedCondition", automation.Condition
)


# Optional: a stick it names (vid / pid) or any stick (0 / 0) gets this mount path; a
# stick without an entry is mounted at the default path. No object per entry - a table
# in flash.
DEVICE_SCHEMA = cv.Schema(
    {
        # Names the entry in the config only (no object behind it)
        cv.Optional(CONF_ID): cv.declare_id(USBStorageEntry),
        cv.Required(CONF_MOUNT_PATH): validate_mount_path,
        cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
        cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
        cv.Optional(CONF_FORMAT_ON_MISMATCH, default=False): cv.boolean,
        cv.Optional(CONF_ON_MOUNTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DeviceMountedTrigger),
            }
        ),
    }
)


def _max_devices(config):
    # Room for every entry at once at least
    entries = len(config.get(CONF_DEVICES) or [])
    if config[CONF_MAX_DEVICES] < entries:
        raise cv.Invalid(
            f"max_devices ({config[CONF_MAX_DEVICES]}) is below the {entries} device entries"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBStorageClient),
            # Only exists together with esp32 enable_exfat -- see storage/__init__.py.
            FILE_SYSTEM_SCHEMA_ENTRY: validate_file_system_value,
            cv.Optional(CONF_DEVICES): cv.ensure_list(DEVICE_SCHEMA),
            # Sticks plugged at the same time (FATFS volumes, storage registry slots); nothing of a
            # stick exists before it is plugged in
            cv.Optional(CONF_MAX_DEVICES, default=4): cv.int_range(min=1, max=9),
            # Sticks / readers no client of this component takes (exact vid:pid pairs)
            **USB_BLACKLIST_SCHEMA,
        }
    ),
    _max_devices,
    only_on_variant(
        supported=[
            VARIANT_ESP32H4,
            VARIANT_ESP32P4,
            VARIANT_ESP32S2,
            VARIANT_ESP32S3,
            VARIANT_ESP32S31,
        ]
    ),
)


def _final_validate(config):
    final_validate_file_system(config)
    # The data phase transfers (2 x 32 KB) are DMA buffers; on the ESP32-P4 the USB DMA reaches
    # PSRAM, so they go there instead of internal RAM.
    if get_esp32_variant() == VARIANT_ESP32P4 and "psram" not in fv.full_config.get():
        raise cv.Invalid("usb_storage on the ESP32-P4 needs psram for its transfer buffers")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    require_vfs_dir()
    require_vfs_select()
    # LFN mode/length and volume count are set as user-overridable defaults by the esp32
    # platform once any component calls require_fatfs() -- see
    # _reconcile_vfs_fatfs_sdkconfig(). Override via esp32 framework sdkconfig_options.
    max_devices = config[CONF_MAX_DEVICES]
    require_fatfs(max_devices)
    include_builtin_idf_component("fatfs")
    include_builtin_idf_component("esp_vfs_fat")

    # One MSC client for every stick: a stick's state is made when it is plugged in
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_usb_match(var, config)
    await file_system_to_code(var, config)
    cg.add(var.set_max_devices(max_devices))

    devices = config.get(CONF_DEVICES) or []
    bases = {DEFAULT_MOUNT_PATH}
    rows = []
    for device in devices:
        if device[CONF_VID] or device[CONF_PID]:
            note_usb_whitelist_used()  # a mount entry names its stick
        bases.add(device[CONF_MOUNT_PATH])
        rows.append(
            cg.StructInitializer(
                USBStorageEntry,
                ("vid", device[CONF_VID]),
                ("pid", device[CONF_PID]),
                ("mount_path", device[CONF_MOUNT_PATH]),
                ("format_on_mismatch", device[CONF_FORMAT_ON_MISMATCH]),
            )
        )
    if rows:
        table_id = ID(
            f"{config[CONF_ID]}_entries", is_declaration=True, type=USBStorageEntry
        )
        table = cg.static_const_array(table_id, cg.ArrayInitializer(*rows))
        cg.add(var.set_entries(table, len(rows)))
    for index, device in enumerate(devices):
        for conf in device.get(CONF_ON_MOUNTED, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var, index)
            await automation.build_automation(
                trigger, [(cg.const_char_ptr, "mount_path")], conf
            )

    # Full VFS paths carry the mount point; the storage component sizes its buffers from the
    # paths registered here - the longest a stick can get is a base path with a number.
    for base in bases:
        register_mount_path(base + "9")
    for _ in range(max_devices):
        request_storage_device()
    # Bounded by FATFS long filenames, i.e. by CONFIG_FATFS_MAX_LFN -- resolved at codegen
    # time rather than baked in, so a user who lowers it gets a matching API bound.
    request_fatfs_path_length()
    # USB MSC transfers are self-contained (own endpoint transfers via a FreeRTOS semaphore, no
    # shared bus with other main-loop-driven components) -- task-safe, unlike e.g. SdSpi.
    request_storage_worker(task_safe=True)

    cg.add_define("USE_USB_BULK_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")
    if get_esp32_variant() == VARIANT_ESP32P4:
        # Every USB DMA buffer in PSRAM: the data phase transfers are 32 KB each.
        add_idf_sdkconfig_option("CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM", True)


# Actions: the stick at a mount path (none: the first mounted one)
USB_STORAGE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(USBStorageClient),
        cv.Optional(CONF_MOUNT_PATH, default=""): cv.string,
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
    return cg.new_Pvariable(action_id, template_arg, paren, config[CONF_MOUNT_PATH])


@automation.register_action(
    "usb_storage.unmount",
    UnmountDeviceAction,
    USB_STORAGE_ACTION_SCHEMA,
    synchronous=True,
)
async def usb_storage_unmount_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren, config[CONF_MOUNT_PATH])


LIST_FILES_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(USBStorageClient),
        cv.Optional(CONF_MOUNT_PATH, default=""): cv.string,
        cv.Optional("path", default=""): cv.templatable(cv.string),
    }
)


@automation.register_action(
    "usb_storage.list_files",
    ListFilesAction,
    LIST_FILES_SCHEMA,
    synchronous=True,
)
async def usb_storage_list_files_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent, config[CONF_MOUNT_PATH])
    if "path" in config:
        template_ = await cg.templatable(config["path"], args, cg.const_char_ptr)
        cg.add(var.set_path(template_))
    return var


# Conditions
@automation.register_condition(
    "usb_storage.is_mounted", DeviceMountedCondition, USB_STORAGE_ACTION_SCHEMA
)
async def usb_storage_is_mounted_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren, config[CONF_MOUNT_PATH])
