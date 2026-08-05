import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import storage
from esphome.const import CONF_ID, CONF_PATH, CONF_TYPE

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
CONF_STORAGE_ID = "storage_id"

ENTRY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.string_strict,
        cv.Required(CONF_TYPE): cv.enum(KINDS, lower=True),
        # Relative to the effective storage (per-entry storage_id, else the top-level one) when a
        # storage is set; otherwise a full VFS path resolved through the storage registry.
        cv.Required(CONF_PATH): cv.string,
        cv.Optional(CONF_STORAGE_ID): cv.use_id(storage.PathStorage),
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
            # Default storage for entries that give a relative path and no own storage_id.
            cv.Optional(CONF_STORAGE_ID): cv.use_id(storage.PathStorage),
            cv.Required(CONF_ENTRIES): cv.ensure_list(ENTRY_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _unique_ids,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    default_storage = None
    if storage_id := config.get(CONF_STORAGE_ID):
        default_storage = await cg.get_variable(storage_id)

    for entry in config[CONF_ENTRIES]:
        st = cg.nullptr
        if entry_storage_id := entry.get(CONF_STORAGE_ID):
            st = await cg.get_variable(entry_storage_id)
        elif default_storage is not None:
            st = default_storage
        cg.add(var.add_entry(entry[CONF_ID], entry[CONF_TYPE], entry[CONF_PATH], st))
