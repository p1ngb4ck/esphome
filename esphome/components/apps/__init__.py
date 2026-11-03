from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_INTERLOCK, CONF_ON_START
from esphome.types import ConfigType

CODEOWNERS = ["@p1ngb4ck"]

CONF_ON_STOP = "on_stop"

apps_ns = cg.esphome_ns.namespace("apps")
AppSlot = apps_ns.class_("AppSlot")
StartAction = apps_ns.class_("StartAction", automation.Action, cg.Parented.template(AppSlot))
StopAction = apps_ns.class_("StopAction", automation.Action, cg.Parented.template(AppSlot))
IsRunningCondition = apps_ns.class_(
    "IsRunningCondition", automation.Condition, cg.Parented.template(AppSlot)
)

APP_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AppSlot),
        cv.Optional(CONF_INTERLOCK, default=[]): cv.ensure_list(cv.use_id(AppSlot)),
        cv.Optional(CONF_ON_START): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_STOP): automation.validate_automation(single=True),
    }
)

CONFIG_SCHEMA = cv.ensure_list(APP_SCHEMA)


async def to_code(config: list[ConfigType]) -> None:
    apps = {}
    for conf in config:
        var = cg.new_Pvariable(conf[CONF_ID])
        apps[str(conf[CONF_ID])] = var
        if on_start := conf.get(CONF_ON_START):
            await automation.build_automation(var.get_start_trigger(), [], on_start)
        if on_stop := conf.get(CONF_ON_STOP):
            await automation.build_automation(var.get_stop_trigger(), [], on_stop)
    excluded = {name: set() for name in apps}
    for conf in config:
        name = str(conf[CONF_ID])
        for other in conf[CONF_INTERLOCK]:
            other_name = str(other)
            if other_name == name:
                continue
            excluded[name].add(other_name)
            excluded[other_name].add(name)
    for name, others in excluded.items():
        if not others:
            continue
        table = cg.shared_progmem_array(
            "apps_interlock",
            AppSlot.operator("ptr"),
            cg.ArrayInitializer(*[apps[o] for o in sorted(others)]),
            constexpr=False,
        )
        cg.add(apps[name].set_excluded(table, len(others)))


APP_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(AppSlot)})


@automation.register_action("apps.start", StartAction, APP_ACTION_SCHEMA, synchronous=True)
@automation.register_action("apps.stop", StopAction, APP_ACTION_SCHEMA, synchronous=True)
async def app_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("apps.is_running", IsRunningCondition, APP_ACTION_SCHEMA)
async def app_is_running_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
