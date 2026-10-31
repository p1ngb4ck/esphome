from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32H4,
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32S31,
    only_on_variant,
)
from esphome.components.storage import (
    CONF_MOUNT_PATH,
    register_mount_path,
    request_iso9660,
    request_storage_device,
    request_storage_worker,
    validate_mount_path,
)
from esphome.components.usb_storage import USBStorageClient
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_storage"]
AUTO_LOAD = ["storage"]

CONF_USB_STORAGE_ID = "usb_storage_id"
CONF_MAX_DRIVES = "max_drives"
CONF_MEDIA_CHECK_INTERVAL = "media_check_interval"
CONF_ON_MEDIA_MOUNTED = "on_media_mounted"
CONF_ON_MEDIA_REMOVED = "on_media_removed"

usb_cdrom_ns = cg.esphome_ns.namespace("usb_cdrom")
USBCdrom = usb_cdrom_ns.class_("USBCdrom", cg.Component)
EjectAction = usb_cdrom_ns.class_("EjectAction", automation.Action)
LoadAction = usb_cdrom_ns.class_("LoadAction", automation.Action)

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBCdrom),
            cv.GenerateID(CONF_USB_STORAGE_ID): cv.use_id(USBStorageClient),
            cv.Optional(CONF_MOUNT_PATH, default="/cdrom"): cv.All(
                validate_mount_path, cv.Length(max=29)
            ),
            cv.Optional(CONF_MAX_DRIVES, default=1): cv.int_range(min=1, max=9),
            cv.Optional(
                CONF_MEDIA_CHECK_INTERVAL, default="2s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_MEDIA_MOUNTED): automation.validate_automation({}),
            cv.Optional(CONF_ON_MEDIA_REMOVED): automation.validate_automation({}),
        }
    ),
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


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    client = await cg.get_variable(config[CONF_USB_STORAGE_ID])
    cg.add(client.set_cdrom(var))
    cg.add(var.set_mount_path(config[CONF_MOUNT_PATH]))
    cg.add(var.set_max_drives(config[CONF_MAX_DRIVES]))
    cg.add(
        var.set_media_check_interval(
            config[CONF_MEDIA_CHECK_INTERVAL].total_milliseconds
        )
    )
    cg.add_define("USE_USB_CDROM")
    request_iso9660()
    for _ in range(config[CONF_MAX_DRIVES]):
        request_storage_device()
    register_mount_path(config[CONF_MOUNT_PATH] + "9")
    request_storage_worker(task_safe=True)
    for conf in config.get(CONF_ON_MEDIA_MOUNTED, []):
        await automation.build_callback_automation(
            var, "add_on_media_mounted_callback", [(cg.const_char_ptr, "mount_path")], conf
        )
    for conf in config.get(CONF_ON_MEDIA_REMOVED, []):
        await automation.build_callback_automation(
            var, "add_on_media_removed_callback", [(cg.const_char_ptr, "mount_path")], conf
        )


USB_CDROM_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(USBCdrom),
        cv.Optional(CONF_MOUNT_PATH, default=""): cv.string,
    }
)


@automation.register_action(
    "usb_cdrom.eject", EjectAction, USB_CDROM_ACTION_SCHEMA, synchronous=True
)
async def usb_cdrom_eject_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent, config[CONF_MOUNT_PATH])


@automation.register_action(
    "usb_cdrom.load", LoadAction, USB_CDROM_ACTION_SCHEMA, synchronous=True
)
async def usb_cdrom_load_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent, config[CONF_MOUNT_PATH])
