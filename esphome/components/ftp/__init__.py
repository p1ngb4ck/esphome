import esphome.codegen as cg
from esphome.components import cert_store
from esphome.components.storage import (
    MountableStorage,
    register_mount_path,
    request_path_length,
    request_storage_device,
    request_storage_worker,
    validate_mount_path,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_PORT, CONF_PROTOCOL, CONF_USERNAME, PLATFORM_ESP32
from esphome.core import CORE

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["network"]
DOMAIN = "ftp"

CONF_CLIENTS = "clients"
CONF_SERVER = "server"
CONF_MOUNT_PATH = "mount_path"
CONF_AUTO_CONNECT = "auto_connect"
CONF_AUTH_TLS = "auth_tls"
CONF_ROOT = "root"
CONF_USERS = "users"
CONF_ACCESS = "access"
CONF_MAX_SESSIONS = "max_sessions"
CONF_TFTP = "tftp"
CONF_MAX_FILE_SIZE = "max_file_size"

PROTOCOL_FTP = "ftp"
PROTOCOL_TFTP = "tftp"
ACCESS = {"read_only": False, "read_write": True}


def AUTO_LOAD(config=None) -> list[str]:
    loads = ["storage", "socket"]
    if isinstance(config, dict) and any(c.get(CONF_AUTH_TLS) for c in config.get(CONF_CLIENTS, [])):
        loads.append("cert_store")
    return loads


ftp_ns = cg.esphome_ns.namespace("ftp")
FTPClient = ftp_ns.class_("FTPClient", cg.Component, MountableStorage)
FtpServer = ftp_ns.class_("FtpServer", cg.Component)


def _validate_client(config):
    if config[CONF_PROTOCOL] == PROTOCOL_TFTP:
        for key in (CONF_USERNAME, CONF_PASSWORD, CONF_AUTH_TLS):
            if config.get(key):
                raise cv.Invalid(f"'{key}' is not used with protocol: tftp")
        if cert_store.tls_keys_present(config):
            raise cv.Invalid("TFTP has no TLS")
        if not CORE.is_esp32:
            raise cv.Invalid("protocol: tftp needs the ESP32 (BSD sockets)")
        return config
    for key in (CONF_USERNAME, CONF_PASSWORD):
        if key not in config:
            raise cv.Invalid(f"'{key}' is required with protocol: ftp")
    if config.get(CONF_AUTH_TLS):
        if not CORE.using_esp_idf:
            raise cv.Invalid("auth_tls (FTPS) requires the esp-idf framework")
    elif cert_store.tls_keys_present(config):
        raise cv.Invalid("ca / client_certificate / client_certificate_key are only used with auth_tls: true")
    return config


def _default_port(config):
    if CONF_PORT not in config:
        config[CONF_PORT] = 69 if config[CONF_PROTOCOL] == PROTOCOL_TFTP else 21
    return config


CLIENT_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FTPClient),
            cv.Optional(CONF_PROTOCOL, default=PROTOCOL_FTP): cv.one_of(PROTOCOL_FTP, PROTOCOL_TFTP, lower=True),
            cv.Required(CONF_SERVER): cv.string,
            cv.Optional(CONF_USERNAME): cv.string,
            cv.Optional(CONF_PASSWORD): cv.string,
            cv.Optional(CONF_PORT): cv.port,
            cv.Required(CONF_MOUNT_PATH): validate_mount_path,
            cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
            cv.Optional(CONF_AUTH_TLS, default=False): cv.boolean,
        }
    )
    .extend(cert_store.TLS_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cert_store.validate_tls,
    _validate_client,
    _default_port,
)


def _storage_root(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or (len(value) > 1 and value.endswith("/")):
        raise cv.Invalid("a storage path, as /sdcard")
    return value


USER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_USERNAME): cv.string_strict,
        cv.Required(CONF_PASSWORD): cv.string_strict,
        cv.Optional(CONF_ACCESS, default="read_only"): cv.one_of(*ACCESS, lower=True),
    }
)

TFTP_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_PORT, default=69): cv.port,
        cv.Optional(CONF_ACCESS, default="read_only"): cv.one_of(*ACCESS, lower=True),
        cv.Optional(CONF_MAX_FILE_SIZE, default="1MB"): cv.All(cv.validate_bytes, cv.Range(min=512, max=33554432)),
    }
)

SERVER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FtpServer),
        cv.Optional(CONF_PORT, default=21): cv.port,
        cv.Required(CONF_ROOT): _storage_root,
        cv.Required(CONF_USERS): cv.All(cv.ensure_list(USER_SCHEMA), cv.Length(min=1, max=8)),
        cv.Optional(CONF_MAX_SESSIONS, default=2): cv.int_range(min=1, max=4),
        cv.Optional(CONF_TFTP): TFTP_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_CLIENTS, default=[]): cv.ensure_list(CLIENT_SCHEMA),
            cv.Optional(CONF_SERVER): cv.All(SERVER_SCHEMA, cv.only_on([PLATFORM_ESP32])),
        }
    ),
    cv.has_at_least_one_key(CONF_CLIENTS, CONF_SERVER),
)


async def _client_to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_server(config[CONF_SERVER]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
    if config[CONF_PROTOCOL] == PROTOCOL_TFTP:
        cg.add_define("USE_FTP_TFTP_CLIENT")
        cg.add(var.set_tftp(True))
    else:
        cg.add(var.set_username(config[CONF_USERNAME]))
        cg.add(var.set_password(config[CONF_PASSWORD]))
    if config[CONF_AUTH_TLS]:
        cg.add_define("USE_FTP_CLIENT_TLS")
        cg.add(var.set_auth_tls(True))
        cg.add(var.set_tls_refs(cert_store.tls_refs(config)))
    cg.add(var.set_mount_path(config[CONF_MOUNT_PATH]))
    register_mount_path(config[CONF_MOUNT_PATH])
    request_storage_device()


async def _server_to_code(config):
    cg.add_define("USE_FTP_SERVER")
    var = cg.Pvariable(config[CONF_ID], cg.MockObj(str(FtpServer), "::").create())
    await cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_root(config[CONF_ROOT]))
    cg.add(var.set_max_sessions(config[CONF_MAX_SESSIONS]))
    for user in config[CONF_USERS]:
        cg.add(var.add_user(user[CONF_USERNAME], user[CONF_PASSWORD], ACCESS[user[CONF_ACCESS]]))
    if (tftp := config.get(CONF_TFTP)) is not None:
        cg.add_define("USE_FTP_TFTP_SERVER")
        cg.add(var.set_tftp(tftp[CONF_PORT], ACCESS[tftp[CONF_ACCESS]], tftp[CONF_MAX_FILE_SIZE]))


async def to_code(config):
    for client in config[CONF_CLIENTS]:
        await _client_to_code(client)
    if (server := config.get(CONF_SERVER)) is not None:
        await _server_to_code(server)
    request_path_length(256)
    request_storage_worker(task_safe=True)
