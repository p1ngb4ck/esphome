"""Paired Bluetooth Classic and LE devices of bt_hid and ble_client_hid slots.

Kept across reboots in preferences, or in a file on a storage (file: a VFS path). The bond
keys themselves stay in Bluedroid's NVS store; forgetting a device also deletes its bond.
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_FILE, CONF_ID
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]

CONF_BT_PAIRING_ID = "bt_pairing_id"
CONF_MAX_DEVICES = "max_devices"

bt_pairing_ns = cg.esphome_ns.namespace("bt_pairing")
BTPairing = bt_pairing_ns.class_("BTPairing", cg.Component)
ForgetAllAction = bt_pairing_ns.class_(
    "ForgetAllAction", automation.Action, cg.Parented.template(BTPairing)
)
ForgetAction = bt_pairing_ns.class_(
    "ForgetAction", automation.Action, cg.Parented.template(BTPairing)
)
LogAction = bt_pairing_ns.class_(
    "LogAction", automation.Action, cg.Parented.template(BTPairing)
)


def _validate_file(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/") or value.count("/") < 2:
        raise cv.Invalid(
            "file is a path under a storage's mount_path, e.g. /sdcard/bt_pairing.bin"
        )
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BTPairing),
        cv.Optional(CONF_MAX_DEVICES, default=8): cv.int_range(min=1, max=32),
        # A file under a storage's mount_path; without it the database is kept in
        # preferences. Read once the storage is mounted, rewritten on every change.
        cv.Optional(CONF_FILE): _validate_file,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_max_devices(config[CONF_MAX_DEVICES]))
    cg.add(var.set_preference_key(fnv1_hash(f"bt_pairing:{config[CONF_ID]}")))
    if CONF_FILE in config:
        cg.add_define("USE_BT_PAIRING_STORAGE")
        cg.add(var.set_file(config[CONF_FILE]))


def _final_validate(config):
    if CONF_FILE in config and "storage" not in fv.full_config.get():
        raise cv.Invalid(
            f"{CONF_FILE} needs a storage (sd_storage, usb_storage, ...) mounted there"
        )


FINAL_VALIDATE_SCHEMA = _final_validate


PAIRING_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(BTPairing)}
)


@automation.register_action(
    "bt_pairing.forget_all",
    ForgetAllAction,
    PAIRING_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "bt_pairing.log", LogAction, PAIRING_ACTION_SCHEMA, synchronous=True
)
async def pairing_simple_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


def _validate_address(value):
    if isinstance(value, cv.Lambda):
        return value
    return str(cv.mac_address(value))


@automation.register_action(
    "bt_pairing.forget",
    ForgetAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(BTPairing),
            cv.Required(CONF_ADDRESS): cv.templatable(_validate_address),
        }
    ),
    synchronous=True,
)
async def forget_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(
        var.set_address(await cg.templatable(config[CONF_ADDRESS], args, cg.std_string))
    )
    return var
