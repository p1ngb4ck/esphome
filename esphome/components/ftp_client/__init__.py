import esphome.codegen as cg
from esphome.components.storage import (
    MountableStorage,
    register_mount_path,
    request_path_length,
    request_storage_device,
    request_storage_worker,
    validate_mount_path,
)
from esphome.components import cert_store
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_USERNAME,
    PLATFORM_BK72XX,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
    PLATFORM_LN882X,
    PLATFORM_RP2040,
    PLATFORM_RTL87XX,
)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["network"]
def AUTO_LOAD(config):
    base = ["storage", "socket"]
    if config.get(CONF_AUTH_TLS):
        base.append("cert_store")
    return base

CONF_SERVER = "server"
CONF_MOUNT_PATH = "mount_path"
CONF_AUTO_CONNECT = "auto_connect"

ftp_client_ns = cg.esphome_ns.namespace("ftp_client")
# MountableStorage parent makes the ftp id a valid target for the generic storage.mount /
# storage.unmount actions (cv.use_id(MountableStorage) checks declared Python parents).
FTPClient = ftp_client_ns.class_("FTPClient", cg.Component, MountableStorage)
CONF_AUTH_TLS = "auth_tls"
CONF_CA = "ca"


def _validate_ftps(config):
    # FTPS is mbedTLS (esp-idf only). When it is on, a CA must be given -- the server is always
    # verified against it -- and it is meaningless otherwise.
    from esphome.core import CORE

    if config.get(CONF_AUTH_TLS):
        if not CORE.using_esp_idf:
            raise cv.Invalid("auth_tls (FTPS) requires the esp-idf framework")
        if CONF_CA not in config:
            raise cv.Invalid("auth_tls: true requires 'ca' (a cert_store entry id)")
    elif CONF_CA in config:
        raise cv.Invalid("'ca' is only used with auth_tls: true")
    return config

DEFAULT_PORT = 21

# username / password accept !secret exactly like the wifi password (resolved at YAML load;
# the C++ never logs the password). Supported on every platform the socket component supports;
# the DNS path is selected in C++ by the socket backend define (getaddrinfo on bsd/lwip_sockets,
# lwip dns_gethostbyname on lwip_tcp).
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
            cv.Optional(CONF_AUTH_TLS, default=False): cv.boolean,
            cv.Optional(CONF_CA): cv.string,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on(
        [
            PLATFORM_BK72XX,
            PLATFORM_ESP32,
            PLATFORM_ESP8266,
            PLATFORM_HOST,
            PLATFORM_LN882X,
            PLATFORM_RP2040,
            PLATFORM_RTL87XX,
        ]
    ),
)


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _validate_ftps)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_server(config[CONF_SERVER]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))

    if config[CONF_AUTH_TLS]:
        cg.add(var.set_auth_tls(True))
        cg.add(var.set_ca_entry(config[CONF_CA]))

    cg.add(var.set_mount_path(config[CONF_MOUNT_PATH]))
    # Full VFS paths carry the mount point; the storage component sizes its buffers from the
    # paths registered here.
    register_mount_path(config[CONF_MOUNT_PATH])

    request_storage_device()
    request_path_length(256)
    request_storage_worker(task_safe=True)
