import esphome.codegen as cg
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PATH, CONF_TYPE
from esphome.core import CORE

CODEOWNERS = ["@p1ngb4ck"]

cert_store_ns = cg.esphome_ns.namespace("cert_store")
CertStore = cert_store_ns.class_("CertStore", cg.Component)
CertKind = cert_store_ns.enum("CertKind", is_class=True)

# YAML type -> C++ CertKind. The bytes are always kept raw; the kind only drives validation and
# documents intent for the consuming component.
KINDS = {
    "ca": CertKind.CA,
    "client_cert": CertKind.CLIENT_CERT,
    "private_key": CertKind.PRIVATE_KEY,
    "psk": CertKind.PSK,
    "ssh_known_host": CertKind.SSH_KNOWN_HOST,
    "ssh_user_key": CertKind.SSH_USER_KEY,
    "raw": CertKind.RAW,
}

CONF_ENTRIES = "entries"

ENTRY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.string_strict,
        cv.Required(CONF_TYPE): cv.enum(KINDS, lower=True),
        # A full VFS path (e.g. /sdcard/certs/ca.pem). The storage registry resolves which storage
        # it lives on -- there is exactly one of each, so no storage is named here.
        cv.Required(CONF_PATH): cv.string,
    }
)


def _unique_ids(config):
    seen = set()
    for entry in config[CONF_ENTRIES]:
        if entry[CONF_ID] in seen:
            raise cv.Invalid(f"Duplicate cert_store entry id '{entry[CONF_ID]}'")
        seen.add(entry[CONF_ID])
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CertStore),
            cv.Required(CONF_ENTRIES): cv.ensure_list(ENTRY_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _unique_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Reads go through the storage worker (the async part of the storage interface), never a
    # blocking main-loop read.
    request_storage_worker()

    # Consumers guard their cert_store use with this. Only on esp-idf, where the store (mbedTLS)
    # actually compiles.
    if CORE.using_esp_idf:
        cg.add_define("USE_CERT_STORE")

    for entry in config[CONF_ENTRIES]:
        cg.add(var.add_entry(entry[CONF_ID], entry[CONF_TYPE], entry[CONF_PATH]))
