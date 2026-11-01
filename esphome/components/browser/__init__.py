from esphome import automation
import esphome.codegen as cg
from esphome.components.remote_desktop import PROTOCOLS, RemoteDesktop
import esphome.config_validation as cv
from esphome.const import CONF_HOST, CONF_ID, CONF_PASSWORD, CONF_PORT, CONF_PROTOCOL

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["remote_desktop"]

CONF_REMOTE_DESKTOP_ID = "remote_desktop_id"

browser_ns = cg.esphome_ns.namespace("browser")
Browser = browser_ns.class_("Browser", cg.Component)
OpenAction = browser_ns.class_("OpenAction", automation.Action)
CloseAction = browser_ns.class_("CloseAction", automation.Action)
IsOpenCondition = browser_ns.class_("IsOpenCondition", automation.Condition)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Browser),
        cv.GenerateID(CONF_REMOTE_DESKTOP_ID): cv.use_id(RemoteDesktop),
        cv.Optional(CONF_PROTOCOL, default="auto"): cv.one_of(*PROTOCOLS, lower=True),
        cv.Required(CONF_HOST): cv.string_strict,
        cv.Optional(CONF_PORT, default=5900): cv.port,
        cv.Optional(CONF_PASSWORD, default=""): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_define("USE_BROWSER")
    var = cg.Pvariable(config[CONF_ID], cg.MockObj(str(Browser), "::").create())
    await cg.register_component(var, config)
    cg.add(var.set_remote_desktop(await cg.get_variable(config[CONF_REMOTE_DESKTOP_ID])))
    cg.add(var.set_protocol(PROTOCOLS[config[CONF_PROTOCOL]]))
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_password(config[CONF_PASSWORD]))


BROWSER_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(Browser)})


@automation.register_action("browser.open", OpenAction, BROWSER_ACTION_SCHEMA, synchronous=True)
@automation.register_action("browser.close", CloseAction, BROWSER_ACTION_SCHEMA, synchronous=True)
async def browser_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("browser.is_open", IsOpenCondition, BROWSER_ACTION_SCHEMA)
async def browser_is_open_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
