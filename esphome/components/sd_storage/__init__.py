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
SdSpi = sd_storage_ns.class_("SdSpi", SdStorageBase)

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
CONF_SPI_INTERFACE = "spi_interface"

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


def validate_spi_bus_exclusive(config):
    """Ensure SD SPI is not sharing the SPI bus with other devices.

    Similar to SPI Ethernet, SD card requires exclusive access to the SPI peripheral
    because ESP-IDF's SDSPI driver takes full control of the bus."""
    # Check that required pins are specified
    if not config.get(CONF_CLK_PIN):
        raise cv.Invalid(f"{CONF_CLK_PIN} is required for SPI mode")
    if not config.get(CONF_CMD_PIN):
        raise cv.Invalid(f"{CONF_CMD_PIN} (MOSI) is required for SPI mode")
    if not config.get(CONF_DATA0_PIN):
        raise cv.Invalid(f"{CONF_DATA0_PIN} (MISO) is required for SPI mode")

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

SD_SPI_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SdSpi),
        cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_CMD_PIN): pins.internal_gpio_output_pin_number,  # MOSI
        cv.Required(CONF_DATA0_PIN): pins.internal_gpio_pin_number,  # MISO
        cv.Optional(
            CONF_DATA3_PIN
        ): pins.internal_gpio_output_pin_schema,  # Alias for CS pin
        cv.Optional(CONF_CS_PIN): pins.internal_gpio_output_pin_schema,
        cv.Optional(CONF_MODE_1BIT, default=True): cv.boolean,
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
        validate_spi_bus_exclusive,
        validate_spi_mode,
    ],
).extend(cv.COMPONENT_SCHEMA)

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


def _final_validate_spi_interface(config):
    """Validate SPI interface usage for SD SPI mode.

    SD SPI can use either hardware SPI or software SPI (bit-banging):
    - Hardware SPI: Faster but requires dedicated SPI peripheral
    - Software SPI: Slower but can use any GPIO pins

    This validation warns if user tries to share hardware SPI with other devices."""
    if config[CONF_TYPE] != TYPE_SD_SPI:
        return

    from esphome.components.esp32 import get_esp32_variant
    from esphome.components.esp32.const import (
        VARIANT_ESP32C3,
        VARIANT_ESP32S2,
        VARIANT_ESP32S3,
    )

    # Determine default SPI host based on ESP32 variant
    variant = get_esp32_variant()
    if variant in (VARIANT_ESP32C3, VARIANT_ESP32S2, VARIANT_ESP32S3):
        sd_spi_host = "SPI2_HOST"
    else:
        # ESP32 classic, ESP32-P4, ESP32-C6, etc. - use SPI3 by default
        sd_spi_host = "SPI3_HOST"

    # Store the SPI interface for use in to_code()
    config[CONF_SPI_INTERFACE] = sd_spi_host

    # Warn if spi: component is using the same hardware SPI interface
    # User can still proceed - ESP-IDF will fall back to software SPI if needed
    if spi_configs := fv.full_config.get().get(CONF_SPI):
        for spi_conf in spi_configs:
            if (index := spi_conf.get(spi.CONF_INTERFACE_INDEX)) is not None:
                interface = spi.get_spi_interface(index)
                if interface == sd_spi_host:
                    raise cv.Invalid(
                        f"SD card is configured to use hardware SPI interface '{sd_spi_host}', "
                        f"but the `spi:` component is also using this interface. "
                        f"Options: "
                        f"(1) Change `interface` on `spi:` component to use a different SPI bus, "
                        f"(2) Remove `spi:` component if only using SD card, "
                        f"(3) Use different pins for SD card to enable software SPI mode (slower but works with any pins)."
                    )


FINAL_VALIDATE_SCHEMA = _final_validate_spi_interface


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    card_type = config[CONF_TYPE]
    if card_type == TYPE_SD_SPI:
        # SPI mode configuration
        # Note: SD SPI uses ESP-IDF's SDSPI driver which requires exclusive bus access
        # We do NOT use spi.register_spi_device() because we're not sharing the bus
        cg.add_define("USE_SD_STORAGE_SPI")

        # Set SPI pins directly (SDSPI driver will configure them)
        cg.add(var.set_clk_pin(config[CONF_CLK_PIN]))
        cg.add(var.set_mosi_pin(config[CONF_CMD_PIN]))  # CMD pin = MOSI
        cg.add(var.set_miso_pin(config[CONF_DATA0_PIN]))  # DATA0 pin = MISO

        # Set CS pin
        cs_pin = await cg.gpio_pin_expression(config[CONF_CS_PIN])
        cg.add(var.set_cs_pin(cs_pin))

        cg.add(var.set_slot(config[CONF_SLOT]))

        # Set mode (must be 1-bit for SPI)
        if mode_1bit := config.get(CONF_MODE_1BIT):
            cg.add(var.set_mode_1bit(mode_1bit))

        # Set SPI interface (determined by final_validate based on ESP32 variant)
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

    cg.add(var.set_mount_path(config[CONF_PATH]))
    cg.add(var.set_id(str(config[CONF_ID])))
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
