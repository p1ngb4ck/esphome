"""
S3 client storage component for ESPHome.

WARNING: This component is EXPERIMENTAL. The API (both Python configuration
and C++ interfaces) may change at any time without following the normal
breaking changes policy. Use at your own risk.

Once the API is considered stable, this warning will be removed.
"""

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
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["network"]


CONF_ENDPOINT = "endpoint"
CONF_BUCKET = "bucket"
CONF_REGION = "region"
CONF_ACCESS_KEY = "access_key"
CONF_SECRET_KEY = "secret_key"
CONF_MOUNT_PATH = "mount_path"
CONF_PATH_STYLE = "path_style"
CONF_AUTO_CONNECT = "auto_connect"
CONF_TLS = "tls"
CONF_CA = "ca"

s3_client_ns = cg.esphome_ns.namespace("s3_client")
# MountableStorage parent makes s3 ids valid targets for the generic storage.mount /
# storage.unmount actions (cv.use_id(MountableStorage) checks declared Python parents).
S3Client = s3_client_ns.class_("S3Client", cg.Component, MountableStorage)

DEFAULT_PORT_TLS = 443
DEFAULT_PORT_PLAIN = 80


def _default_port(config):
    if CONF_PORT not in config:
        config = dict(config)
        config[CONF_PORT] = (
            DEFAULT_PORT_TLS if config.get(CONF_TLS, True) else DEFAULT_PORT_PLAIN
        )
    return config


def _validate_tls(config):
    # Same contract as ftp_client's auth_tls: TLS requires a CA entry (always verified),
    # and a CA entry without TLS is a configuration error.
    if config.get(CONF_TLS, True):
        if CONF_CA not in config:
            raise cv.Invalid(
                f"'{CONF_CA}' (a cert_store entry id) is required when '{CONF_TLS}' is enabled"
            )
    elif CONF_CA in config:
        raise cv.Invalid(f"'{CONF_CA}' has no effect with '{CONF_TLS}: false'")
    return config


S3_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(S3Client),
        cv.Required(CONF_ENDPOINT): cv.string,
        cv.Optional(CONF_PORT): cv.port,
        cv.Required(CONF_BUCKET): cv.string,
        cv.Optional(CONF_REGION, default="us-east-1"): cv.string,
        cv.Required(CONF_ACCESS_KEY): cv.string,
        cv.Required(CONF_SECRET_KEY): cv.string,
        cv.Required(CONF_MOUNT_PATH): validate_mount_path,
        # MinIO and most self-hosted S3 servers require path-style addressing; AWS accepts it.
        cv.Optional(CONF_PATH_STYLE, default=True): cv.boolean,
        cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
        cv.Optional(CONF_TLS, default=True): cv.boolean,
        cv.Optional(CONF_CA): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


CONFIG_SCHEMA = cv.All(
    cv.ensure_list(S3_SCHEMA, _default_port, _validate_tls),
    # only_with_esp_idf does not exist as a prebuilt alias (only only_with_arduino does);
    # the generic form takes the Framework enum.
    cv.only_with_framework(cv.Framework.ESP_IDF),
)


def _auto_load(config):
    # cert_store carries the CA material; only pulled in when some share uses TLS, so a
    # plain-http LAN MinIO setup does not pay for it.
    loads = ["storage"]
    if any(share.get(CONF_TLS, True) for share in config):
        loads.append("cert_store")
    return loads


AUTO_LOAD = _auto_load


async def to_code(config):
    for share in config:
        var = cg.new_Pvariable(share[CONF_ID])
        await cg.register_component(var, share)

        cg.add(var.set_endpoint(share[CONF_ENDPOINT]))
        cg.add(var.set_port(share[CONF_PORT]))
        cg.add(var.set_bucket(share[CONF_BUCKET]))
        cg.add(var.set_region(share[CONF_REGION]))
        cg.add(var.set_access_key(share[CONF_ACCESS_KEY]))
        cg.add(var.set_secret_key(share[CONF_SECRET_KEY]))
        cg.add(var.set_path_style(share[CONF_PATH_STYLE]))
        cg.add(var.set_auto_connect(share[CONF_AUTO_CONNECT]))
        if share[CONF_TLS]:
            cg.add(var.set_tls(True))
            cg.add(var.set_ca_entry(share[CONF_CA]))
        else:
            cg.add(var.set_tls(False))

        cg.add(var.set_mount_path(share[CONF_MOUNT_PATH]))
        # Full VFS paths carry the mount point; the storage component sizes its buffers from
        # the paths registered here.
        register_mount_path(share[CONF_MOUNT_PATH])

        request_storage_device()
        # Deliberately the system default (256): S3 allows 1024-byte keys, but the path bound
        # is a global maximum every pool, walk and consumer buffer pays for. Keys are bounded
        # by 256 minus the mount path; raising this waits until the consumers are ready for it.
        request_path_length(256)
        # All I/O is our own socket + mbedTLS under the worker's serialization -- safe on the
        # background task.
        request_storage_worker(task_safe=True)
