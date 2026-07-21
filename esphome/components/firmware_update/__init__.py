from esphome import automation
import esphome.codegen as cg
from esphome.components.ota import BASE_OTA_SCHEMA, OTAComponent, ota_to_code
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PATH
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority

CODEOWNERS = ["@p1ngb4ck"]

# The flash engine (app slot, partition table, combined pre-fill routing) is provided by the ota
# component and compiled once there; this platform links against it instead of duplicating it.
DEPENDENCIES = ["storage"]
AUTO_LOAD = ["md5", "ota"]

CONF_ALLOW_PARTITION_ACCESS = "allow_partition_access"

firmware_update_ns = cg.esphome_ns.namespace("firmware_update")
FirmwareUpdateComponent = firmware_update_ns.class_("FirmwareUpdateComponent", OTAComponent)
FirmwareUpdateFlashAction = firmware_update_ns.class_(
    "FirmwareUpdateFlashAction", automation.Action
)


def _validate_local_path(value):
    """The firmware image lives on mounted storage, addressed by an absolute POSIX path
    (/sdcard/firmware.bin). file:// is accepted as an optional backward-compatible alias and
    left in place for the reader to strip."""
    value = cv.string_strict(value)
    if not value.startswith("file://") and not value.startswith("/"):
        raise cv.Invalid("path must be an absolute POSIX path (e.g. /sdcard/firmware.bin)")
    return value


def _validate_partition_access(config):
    if config.get(CONF_ALLOW_PARTITION_ACCESS) and not CORE.is_esp32:
        raise cv.Invalid(f"{CONF_ALLOW_PARTITION_ACCESS} is only supported on the esp32")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FirmwareUpdateComponent),
            cv.Optional(CONF_PATH): _validate_local_path,
            cv.Optional(CONF_ALLOW_PARTITION_ACCESS, default=False): cv.boolean,
        }
    )
    .extend(BASE_OTA_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    _validate_partition_access,
)


@coroutine_with_priority(CoroPriority.OTA_UPDATES)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await ota_to_code(var, config)
    await cg.register_component(var, config)

    if path := config.get(CONF_PATH):
        cg.add(var.set_path(path))

    # Same opt-in as the OTA platform: only a partition-access build compiles the partition
    # table / combined pre-fill image code (a plain app update never needs it).
    if config.get(CONF_ALLOW_PARTITION_ACCESS):
        cg.add_define("USE_OTA_PARTITIONS")


FIRMWARE_UPDATE_FLASH_ACTION_SCHEMA = cv.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(FirmwareUpdateComponent),
        cv.Optional(CONF_PATH): cv.templatable(_validate_local_path),
    }
)


@automation.register_action(
    "firmware_update.flash",
    FirmwareUpdateFlashAction,
    FIRMWARE_UPDATE_FLASH_ACTION_SCHEMA,
    synchronous=True,
)
async def firmware_update_flash_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)

    if path := config.get(CONF_PATH):
        template_ = await cg.templatable(path, args, cg.std_string)
        cg.add(var.set_path(template_))

    return var
