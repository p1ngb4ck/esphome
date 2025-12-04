from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_CLK_PIN,
    CONF_ID,
    CONF_OUTPUT,
    CONF_PATH,
    CONF_PULLDOWN,
    CONF_PULLUP,
    CONF_TRIGGER_ID,
)

CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = []

# Mark that this component requires VFS directory support
# This must be set early so ESP32 component can check it during its own to_code()
esp32.require_vfs_dir()

sd_storage_ns = cg.esphome_ns.namespace("sd_storage")
SdMmc = sd_storage_ns.class_("SdMmc", cg.Component)

# Automation classes
CardMountedTrigger = sd_storage_ns.class_(
    "CardMountedTrigger", automation.Trigger.template(cg.std_string)
)
MountCardAction = sd_storage_ns.class_("MountCardAction", automation.Action)
UnmountCardAction = sd_storage_ns.class_("UnmountCardAction", automation.Action)
ListFilesAction = sd_storage_ns.class_("ListFilesAction", automation.Action)
CardMountedCondition = sd_storage_ns.class_(
    "CardMountedCondition", automation.Condition
)

CONF_CMD_PIN = "cmd_pin"
CONF_DATA0_PIN = "data0_pin"
CONF_DATA1_PIN = "data1_pin"
CONF_DATA2_PIN = "data2_pin"
CONF_DATA3_PIN = "data3_pin"
CONF_MODE_1BIT = "mode_1bit"
CONF_CS_PIN = "cs_pin"
CONF_SLOT = "slot"
CONF_ON_MOUNTED = "on_mounted"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SdMmc),
        cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_CMD_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_DATA0_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_DATA1_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_DATA2_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_DATA3_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_MODE_1BIT, default=False): cv.boolean,
        cv.Optional(CONF_CS_PIN): pins.gpio_pin_schema(
            {
                CONF_OUTPUT: True,
                CONF_PULLUP: False,
                CONF_PULLDOWN: False,
            }
        ),
        cv.Optional(CONF_SLOT, default=0): cv.int_range(min=0, max=1),
        cv.Optional(CONF_PATH, default="/sdcard"): cv.string,
        cv.Optional(CONF_ON_MOUNTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CardMountedTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Set mode and slot first
    cg.add(var.set_mode_1bit(config[CONF_MODE_1BIT]))
    cg.add(var.set_slot(config[CONF_SLOT]))

    # Set pins
    cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_cmd_pin(config[CONF_CMD_PIN]))
    cg.add(var.set_data0_pin(config[CONF_DATA0_PIN]))

    # Only set data pins if not in 1-bit mode
    if not config[CONF_MODE_1BIT]:
        if CONF_DATA1_PIN in config:
            cg.add(var.set_data1_pin(config[CONF_DATA1_PIN]))
        if CONF_DATA2_PIN in config:
            cg.add(var.set_data2_pin(config[CONF_DATA2_PIN]))
        if CONF_DATA3_PIN in config:
            cg.add(var.set_data3_pin(config[CONF_DATA3_PIN]))

    if CONF_CS_PIN in config:
        cs_pin = config[CONF_CS_PIN]
        cs_ctrl = await cg.gpio_pin_expression(cs_pin)
        cg.add(var.set_cs_pin(cs_ctrl))

    # Set mount path
    cg.add(var.set_mount_path(config[CONF_PATH]))

    # Set ID for storage registry
    cg.add(var.set_id(str(config[CONF_ID])))

    # Store device reference in CORE.data for storage to access
    # This allows storage to register callbacks with SD storage
    from esphome.core import CORE

    if not hasattr(CORE, "data"):
        CORE.data = {}
    if "sd_storage_devices" not in CORE.data:
        CORE.data["sd_storage_devices"] = []
    CORE.data["sd_storage_devices"].append(var)

    # Register on_mounted trigger
    for conf in config.get(CONF_ON_MOUNTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "mount_path")], conf
        )


# Actions
SD_STORAGE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(SdMmc),
    }
)


@automation.register_action(
    "sd_storage.mount", MountCardAction, SD_STORAGE_ACTION_SCHEMA
)
async def sd_storage_mount_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "sd_storage.unmount", UnmountCardAction, SD_STORAGE_ACTION_SCHEMA
)
async def sd_storage_unmount_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


LIST_FILES_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(SdMmc),
        cv.Optional("path", default=""): cv.templatable(cv.string),
    }
)


@automation.register_action("sd_storage.list_files", ListFilesAction, LIST_FILES_SCHEMA)
async def sd_storage_list_files_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    if "path" in config:
        template_ = await cg.templatable(config["path"], args, cg.std_string)
        cg.add(var.set_path(template_))
    return var


# Conditions
@automation.register_condition(
    "sd_storage.is_mounted", CardMountedCondition, SD_STORAGE_ACTION_SCHEMA
)
async def sd_storage_is_mounted_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)
