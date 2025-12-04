from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import esp32, spi
import esphome.config_validation as cv
from esphome.const import (
    CONF_CLK_PIN,
    CONF_CS_PIN,
    CONF_ID,
    CONF_PATH,
    CONF_SPI,
    CONF_SPI_ID,
    CONF_TRIGGER_ID,
    CONF_TYPE,
)
from esphome.core import CORE
import esphome.final_validate as fv

CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = []

# Mark that this component requires VFS directory support
# This must be set early so ESP32 component can check it during its own to_code()
esp32.require_vfs_dir()

sd_storage_ns = cg.esphome_ns.namespace("sd_storage")
SdStorageBase = sd_storage_ns.class_("SdStorageBase", cg.Component)
SdMmc = sd_storage_ns.class_("SdMmc", SdStorageBase)
SdSpi = sd_storage_ns.class_("SdSpi", spi.SPIDevice, SdStorageBase)

# Automation classes (templated to work with both SdMmc and SdSpi)
# The actual C++ classes are templates, so we don't specify the parent type here
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
CONF_SLOT = "slot"
CONF_ON_MOUNTED = "on_mounted"
CONF_SPI_INTERFACE = "_spi_interface"

TYPE_SD_MMC = "sd_mmc"
TYPE_SD_SPI = "sd_spi"

TYPE_CLASS = {
    TYPE_SD_MMC: SdMmc,
    TYPE_SD_SPI: SdSpi,
}


def validate_spi_cs_config(config):
    """Validate CS pin configuration - allow data3_pin as alias for cs_pin in SPI mode."""
    data3_pin_config = config.get(CONF_DATA3_PIN)
    cs_pin_config = config.get(CONF_CS_PIN)
    if data3_pin_config and cs_pin_config:
        raise cv.Invalid(
            f"{CONF_DATA3_PIN} is the same as {CONF_CS_PIN}. Please remove one."
        )
    if not data3_pin_config and not cs_pin_config:
        raise cv.Invalid(
            f"{CONF_DATA3_PIN} or {CONF_CS_PIN} required. Please specify one."
        )
    if data3_pin_config:
        config[CONF_CS_PIN] = data3_pin_config
        del config[CONF_DATA3_PIN]
    return config


def validate_spi_bus_pins(config):
    """Ensure SPI bus pins are defined in SPI component, not in sd_storage."""
    cmd_pin = config.get(CONF_CMD_PIN)
    data0_pin = config.get(CONF_DATA0_PIN)
    clk_pin = config.get(CONF_CLK_PIN)
    if cmd_pin or data0_pin or clk_pin:
        raise cv.Invalid(
            f"Please move pins to SPI bus definition:\n"
            f" '{CONF_CMD_PIN}' to 'spi.mosi_pin'\n"
            f" '{CONF_DATA0_PIN}' to 'spi.miso_pin'\n"
            f" '{CONF_CLK_PIN}' to 'spi.clk_pin'"
        )
    return config


def validate_spi_mode(config):
    """Validate SPI mode requirements."""
    if not CORE.using_esp_idf:
        raise cv.Invalid("Only esp-idf supported for SD SPI")
    if config[CONF_MODE_1BIT] is False:
        raise cv.Invalid("Only 1bit mode supported for SPI")
    return config


def validate_platform_variant(config):
    """Validate platform compatibility."""
    from esphome.components.esp32 import get_esp32_variant
    from esphome.components.esp32.const import VARIANT_ESP32C6

    variant = get_esp32_variant()
    if variant == VARIANT_ESP32C6 and config.get(CONF_TYPE) != TYPE_SD_SPI:
        raise cv.Invalid(
            f"esp32c6 doesn't have sdmmc host support. Please use `type: {TYPE_SD_SPI}`"
        )
    return config


# SDMMC Schema (existing configuration)
SD_MMC_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SdMmc),
        cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_CMD_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_DATA0_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_DATA1_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_DATA2_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_DATA3_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_MODE_1BIT, default=False): cv.boolean,
        cv.Optional(CONF_SLOT, default=0): cv.int_range(min=0, max=1),
        cv.Optional(CONF_PATH, default="/sdcard"): cv.string,
        cv.Optional(CONF_ON_MOUNTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CardMountedTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

# SPI Schema (new configuration)
SD_SPI_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SdSpi),
        cv.Optional(CONF_MODE_1BIT, default=True): cv.boolean,
        cv.Optional(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_CMD_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_DATA0_PIN): pins.internal_gpio_pin_number,
        cv.Optional(CONF_DATA3_PIN): pins.gpio_output_pin_schema,  # Alias for CS pin
        cv.Optional(CONF_SLOT, default=0): cv.int_range(min=0, max=1),
        # DATA1 and DATA2 are not used in standard SD SPI mode (only 1-bit)
        cv.Optional(CONF_PATH, default="/sdcard"): cv.string,
        cv.Optional(CONF_ON_MOUNTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(CardMountedTrigger),
            }
        ),
    },
    extra_schemas=[
        validate_spi_cs_config,
        validate_spi_bus_pins,
        validate_spi_mode,
    ],
).extend(spi.spi_device_schema(cs_pin_required=False))

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            TYPE_SD_MMC: SD_MMC_SCHEMA,
            TYPE_SD_SPI: SD_SPI_SCHEMA,
        },
        default_type=TYPE_SD_MMC,
    ),
    validate_platform_variant,
)


def _final_validate(config):
    """Extract SPI interface index for SPI mode."""
    if config[CONF_TYPE] != TYPE_SD_SPI:
        return

    spi_id = config[CONF_SPI_ID]
    if spi_configs := fv.full_config.get().get(CONF_SPI):
        for spi_conf in spi_configs:
            if spi_conf[CONF_ID] != spi_id:
                continue
            index = spi_conf.get(spi.CONF_INTERFACE_INDEX)
            if index is None:
                raise cv.Invalid(f"Can't find interface index in spi config {spi_id}")

            interface = spi.get_spi_interface(index)
            config[CONF_SPI_INTERFACE] = interface


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    card_type = config[CONF_TYPE]

    if card_type == TYPE_SD_SPI:
        # SPI mode configuration
        cg.add_define("USE_SD_STORAGE_SPI")
        await spi.register_spi_device(var, config)
        cg.add(var.set_slot(config[CONF_SLOT]))
        # Set mode (must be 1-bit for SPI)
        if mode_1bit := config.get(CONF_MODE_1BIT):
            cg.add(var.set_mode_1bit(mode_1bit))

        # Set SPI interface
        if spi_interface := config.get(CONF_SPI_INTERFACE):
            cg.add(var.set_spi_interface(cg.RawExpression(spi_interface)))

    elif card_type == TYPE_SD_MMC:
        # SDMMC mode configuration
        cg.add_define("USE_SD_STORAGE_SDMMC")

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

    # Set mount path (common to both modes)
    cg.add(var.set_mount_path(config[CONF_PATH]))

    # Set ID for storage registry (common to both modes)
    cg.add(var.set_id(str(config[CONF_ID])))

    # Store device reference in CORE.data for storage to access
    # This allows storage to register callbacks with SD storage
    if not hasattr(CORE, "data"):
        CORE.data = {}
    if "sd_storage_devices" not in CORE.data:
        CORE.data["sd_storage_devices"] = []
    CORE.data["sd_storage_devices"].append(var)

    # Register on_mounted trigger (common to both modes)
    for conf in config.get(CONF_ON_MOUNTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_string, "mount_path")], conf
        )


# Actions - these work with both SdMmc and SdSpi via polymorphism
SD_STORAGE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(SdStorageBase),
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
        cv.Required(CONF_ID): cv.use_id(SdStorageBase),
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
