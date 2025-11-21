import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_PORT, CONF_USERNAME

# Constants
CONF_MOUNT_PATH = "mount_path"
CONF_SERVER = "server"

# Namespace
ftp_client_ns = cg.esphome_ns.namespace("ftp_client")
FTPClient = ftp_client_ns.class_("FTPClient", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FTPClient),
        cv.Required(CONF_SERVER): cv.string,
        cv.Required(CONF_USERNAME): cv.string,
        cv.Required(CONF_PASSWORD): cv.string,
        cv.Optional(CONF_PORT, default=21): cv.port,
        cv.Optional(CONF_MOUNT_PATH): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_NETWORK")
    cg.add(var.set_server(config[CONF_SERVER]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_port(config[CONF_PORT]))
    if CONF_MOUNT_PATH in config:
        cg.add(var.set_mount_path(config[CONF_MOUNT_PATH]))
