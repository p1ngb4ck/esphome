import esphome.codegen as cg
from esphome.components.storage import (
    MountableStorage,
    register_mount_path,
    request_path_length,
    request_storage_device,
    request_storage_worker,
    validate_mount_path,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_USERNAME,
    PLATFORM_ESP32,
)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["storage"]

CONF_SERVER = "server"
CONF_MOUNT_PATH = "mount_path"
CONF_AUTO_CONNECT = "auto_connect"

ftp_client_ns = cg.esphome_ns.namespace("ftp_client")
# MountableStorage parent makes the ftp id a valid target for the generic storage.mount /
# storage.unmount actions (cv.use_id(MountableStorage) checks declared Python parents).
FTPClient = ftp_client_ns.class_("FTPClient", cg.Component, MountableStorage)

DEFAULT_PORT = 21

# username / password accept !secret exactly like the wifi password (resolved at YAML load;
# the C++ never logs the password). ESP32 only for now -- the client uses lwip sockets.
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FTPClient),
            cv.Required(CONF_SERVER): cv.string,
            cv.Required(CONF_USERNAME): cv.string,
            cv.Required(CONF_PASSWORD): cv.string,
            cv.Optional(CONF_PORT, default=DEFAULT_PORT): cv.port,
            cv.Required(CONF_MOUNT_PATH): validate_mount_path,
            # Fire one mount attempt on each rising edge of network connectivity; no periodic
            # retry (schedule your own via interval:/automations calling storage.mount).
            cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_server(config[CONF_SERVER]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))

    cg.add(var.set_mount_path(config[CONF_MOUNT_PATH]))
    # Full VFS paths carry the mount point; the storage component sizes its buffers from the
    # paths registered here.
    register_mount_path(config[CONF_MOUNT_PATH])

    request_storage_device()
    request_path_length(256)
    request_storage_worker(task_safe=True)
