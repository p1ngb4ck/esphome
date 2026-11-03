from esphome import automation
import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_KEY, CONF_PATH, CONF_STATE
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.helpers import cpp_string_escape

CODEOWNERS = ["@p1ngb4ck"]


def AUTO_LOAD(config=None) -> list[str]:
    return ["text_sensor"] if isinstance(config, dict) and config.get("state") else []


nvs_ns = cg.esphome_ns.namespace("nvs")
NvsManager = nvs_ns.class_("NvsManager", cg.Component)

CheckAction = nvs_ns.class_("CheckAction", automation.Action, cg.Parented.template(NvsManager))
InvalidateAction = nvs_ns.class_("InvalidateAction", automation.Action, cg.Parented.template(NvsManager))
ResetAction = nvs_ns.class_("ResetAction", automation.Action, cg.Parented.template(NvsManager))
RequestAction = nvs_ns.class_("RequestAction", automation.Action, cg.Parented.template(NvsManager))
BackupAction = nvs_ns.class_("BackupAction", automation.Action, cg.Parented.template(NvsManager))
RestoreAction = nvs_ns.class_("RestoreAction", automation.Action, cg.Parented.template(NvsManager))

CONF_ON_UPGRADE = "on_upgrade"
CONF_ON_DOWNGRADE = "on_downgrade"
CONF_ON_FOREIGN = "on_foreign"
CONF_ON_CORRUPT = "on_corrupt"
CONF_PROTECTED_NAMESPACES = "protected_namespaces"
CONF_ON_CHECK = "on_check"
CONF_NAMESPACE = "namespace"
CONF_KEEP_PROTECTED = "keep_protected"
CONF_PROTECTED_ONLY = "protected_only"
CONF_REBOOT = "reboot"
CONF_POLICY = "policy"

POLICIES = {
    "keep": 0,
    "invalidate_changed": 1,
    "reset": 2,
    "read_only": 3,
    "reset_all": 4,
}
CORRUPT_POLICIES = {"keep": 0, "reset": 2}

DEFAULT_PROTECTED = ["nvs.net80211", "phy", "bt_config.conf", "nimble_bond", "connected_bda"]
NAMESPACE_OWNERS = ("nvs_pins", "nvs", "name_from_nvs", "identity")


def _configured_namespaces(node, parent=None) -> list[str]:
    found = []
    if isinstance(node, dict):
        if parent in NAMESPACE_OWNERS and isinstance(node.get(CONF_NAMESPACE), str):
            found.append(node[CONF_NAMESPACE])
        for key, value in node.items():
            found.extend(_configured_namespaces(value, key))
    elif isinstance(node, list):
        for item in node:
            found.extend(_configured_namespaces(item, parent))
    return found

NVS_NAME = cv.All(cv.string_strict, cv.Length(min=1, max=15))

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NvsManager),
            cv.Optional(CONF_ON_UPGRADE, default="invalidate_changed"): cv.one_of(*POLICIES, lower=True),
            cv.Optional(CONF_ON_DOWNGRADE, default="invalidate_changed"): cv.one_of(*POLICIES, lower=True),
            cv.Optional(CONF_ON_FOREIGN, default="read_only"): cv.one_of(*POLICIES, lower=True),
            cv.Optional(CONF_ON_CORRUPT, default="reset"): cv.one_of(*CORRUPT_POLICIES, lower=True),
            cv.Optional(CONF_PROTECTED_NAMESPACES, default=[]): cv.ensure_list(NVS_NAME),
            cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_ON_CHECK): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


@coroutine_with_priority(CoroPriority.FINAL)
async def _protected_define(own: list[str]) -> None:
    names = []
    for name in [*DEFAULT_PROTECTED, *own, *_configured_namespaces(CORE.config)]:
        if name not in names:
            names.append(name)
    cg.add_define("ESP32_NVS_GUARD_PROTECTED", cg.RawExpression(", ".join(cpp_string_escape(n) for n in names)))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_ESP32_NVS_GUARD")
    cg.add_define("ESP32_NVS_GUARD_ON_UPGRADE", POLICIES[config[CONF_ON_UPGRADE]])
    cg.add_define("ESP32_NVS_GUARD_ON_DOWNGRADE", POLICIES[config[CONF_ON_DOWNGRADE]])
    cg.add_define("ESP32_NVS_GUARD_ON_FOREIGN", POLICIES[config[CONF_ON_FOREIGN]])
    cg.add_define("ESP32_NVS_GUARD_ON_CORRUPT", CORRUPT_POLICIES[config[CONF_ON_CORRUPT]])
    CORE.add_job(_protected_define, config[CONF_PROTECTED_NAMESPACES])
    if state := config.get(CONF_STATE):
        cg.add(var.set_state_sensor(await text_sensor.new_text_sensor(state)))
    if on_check := config.get(CONF_ON_CHECK):
        await automation.build_automation(
            var.get_check_trigger(), [(cg.std_string, "result"), (cg.std_string, "state")], on_check
        )


NVS_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(NvsManager)})


@automation.register_action("nvs.check", CheckAction, NVS_ACTION_SCHEMA, synchronous=True)
async def check_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "nvs.invalidate",
    InvalidateAction,
    NVS_ACTION_SCHEMA.extend(
        {
            cv.Required(CONF_NAMESPACE): cv.templatable(NVS_NAME),
            cv.Optional(CONF_KEY, default=""): cv.templatable(cv.string_strict),
        }
    ),
    synchronous=True,
)
async def invalidate_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_ns(await cg.templatable(config[CONF_NAMESPACE], args, cg.std_string)))
    cg.add(var.set_key(await cg.templatable(config[CONF_KEY], args, cg.std_string)))
    return var


@automation.register_action(
    "nvs.reset",
    ResetAction,
    NVS_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_KEEP_PROTECTED, default=True): cv.boolean,
            cv.Optional(CONF_REBOOT, default=True): cv.boolean,
        }
    ),
    synchronous=True,
)
async def reset_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_keep_protected(config[CONF_KEEP_PROTECTED]))
    cg.add(var.set_reboot(config[CONF_REBOOT]))
    return var


@automation.register_action(
    "nvs.request",
    RequestAction,
    NVS_ACTION_SCHEMA.extend({cv.Required(CONF_POLICY): cv.templatable(cv.one_of(*POLICIES, lower=True))}),
    synchronous=True,
)
async def request_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_policy(await cg.templatable(config[CONF_POLICY], args, cg.std_string)))
    return var


def _needs_storage(config):
    if "storage" not in CORE.loaded_integrations:
        raise cv.Invalid("The file needs a storage (storage:)")
    return config


@automation.register_action(
    "nvs.backup",
    BackupAction,
    cv.All(NVS_ACTION_SCHEMA.extend({cv.Required(CONF_PATH): cv.templatable(cv.string_strict)}), _needs_storage),
    synchronous=True,
)
async def backup_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


@automation.register_action(
    "nvs.restore",
    RestoreAction,
    cv.All(
        NVS_ACTION_SCHEMA.extend(
            {
                cv.Required(CONF_PATH): cv.templatable(cv.string_strict),
                cv.Optional(CONF_PROTECTED_ONLY, default=False): cv.boolean,
                cv.Optional(CONF_REBOOT, default=True): cv.boolean,
            }
        ),
        _needs_storage,
    ),
    synchronous=True,
)
async def restore_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    cg.add(var.set_protected_only(config[CONF_PROTECTED_ONLY]))
    cg.add(var.set_reboot(config[CONF_REBOOT]))
    return var
