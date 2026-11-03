import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32", "network", "http_request"]
AUTO_LOAD = ["json", "fixed_containers"]
DOMAIN = "relay_link"

relay_link_ns = cg.esphome_ns.namespace("relay_link")
RelayLink = relay_link_ns.class_("RelayLink", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(RelayLink)})


async def to_code(config):
    cg.add_define("USE_RELAY_LINK")
    var = cg.Pvariable(config[CONF_ID], cg.MockObj(str(RelayLink), "::").create())
    await cg.register_component(var, config)
    http = CORE.config["http_request"]
    http = http[0] if isinstance(http, list) else http
    cg.add(var.set_http_request(await cg.get_variable(http[CONF_ID])))
    api = CORE.config.get("api")
    if isinstance(api, dict) and api.get("custom_services", False):
        cg.add_define("USE_RELAY_LINK_API")


async def get_relay_link():
    return await cg.get_variable(CORE.config[DOMAIN][CONF_ID])
