from pathlib import Path

from esphome.components import esp32
import esphome.codegen as cg
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PATH, CONF_TYPE
from esphome.core import CORE

CODEOWNERS = ["@p1ngb4ck"]

cert_store_ns = cg.esphome_ns.namespace("cert_store")
CertStore = cert_store_ns.class_("CertStore", cg.Component)
CertKind = cert_store_ns.enum("CertKind", is_class=True)

# YAML type -> C++ CertKind. The kind drives validation and documents intent for the consumer.
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
CONF_SOURCE = "source"

# Where an entry's bytes come from.
SOURCE_EMBEDDED = "embedded"  # a local file, read at compile time into a flash literal
SOURCE_STORAGE = "storage"  # a VFS path on a mounted storage, streamed on demand at runtime
SOURCES = (SOURCE_EMBEDDED, SOURCE_STORAGE)


def _validate_entry(entry):
    # source is normally inferred from the path so the user never sets it:
    #   absolute path (/sd/certs/ca.pem) -> storage: a VFS path resolved on the DEVICE at runtime,
    #                                       so it is never checked on the build host.
    #   relative path (certs/ca.pem)     -> embedded: a local file baked into flash at compile time.
    # An explicit 'source:' overrides the inference (and is then validated for consistency).
    path = entry[CONF_PATH]
    source = entry.get(CONF_SOURCE)
    if source is None:
        source = SOURCE_STORAGE if path.startswith("/") else SOURCE_EMBEDDED
        entry[CONF_SOURCE] = source

    if source == SOURCE_EMBEDDED:
        # Compile-time file, checked on the host.
        if path.startswith("/"):
            raise cv.Invalid(
                f"embedded cert '{entry[CONF_ID]}': '{path}' is an absolute path, which is a device "
                "VFS path -- use 'source: storage', or give a path relative to the config for an "
                "embedded cert"
            )
        if not Path(CORE.relative_config_path(path)).is_file():
            raise cv.Invalid(
                f"embedded cert '{entry[CONF_ID]}': file '{path}' not found relative to the config"
            )
    elif not path.startswith("/"):
        # storage: a device VFS path -- must be absolute, never touched on the host.
        raise cv.Invalid(
            f"storage cert '{entry[CONF_ID]}': path '{path}' must be an absolute VFS path "
            "(e.g. /sd/certs/ca.pem)"
        )
    return entry


ENTRY_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.string_strict,
            cv.Required(CONF_TYPE): cv.enum(KINDS, lower=True),


            # Inferred from the path when unset (absolute -> storage, relative -> embedded);
            # set explicitly only to override that inference.
            cv.Optional(CONF_SOURCE): cv.one_of(*SOURCES, lower=True),
            cv.Required(CONF_PATH): cv.string,
        }
    ),
    _validate_entry,
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
            # Optional: a cert_store with no entries still provides the built-in Mozilla CA
            # bundle as the default trust anchor, so AUTO_LOAD (e.g. from an s3_client using
            # tls without a named ca) can pull in a bundle-only store with no YAML section.
            cv.Optional(CONF_ENTRIES, default=list): cv.ensure_list(ENTRY_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _unique_ids,
    cv.only_with_framework(cv.Framework.ESP_IDF),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Storage-sourced entries stream through the worker (the async part of the storage interface),
    # never a blocking main-loop read.
    request_storage_worker()

    # Consumers guard their cert_store use with this.
    cg.add_define("USE_CERT_STORE")

    # Force the built-in Mozilla CA bundle in: it is the default trust anchor whenever a consumer
    # requests a CA without naming an entry, so it must always be available.
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)

    for entry in config[CONF_ENTRIES]:
        if entry[CONF_SOURCE] == SOURCE_EMBEDDED:
            # Read the file at compile time and bake it into flash as a NUL-terminated literal --
            # mbedTLS parses it straight out of .rodata, no runtime RAM.
            content = Path(CORE.relative_config_path(entry[CONF_PATH])).read_text(
                encoding="utf-8"
            )
            # Bake the PEM into flash as a string literal; mbedTLS parses it straight out of
            # .rodata, no runtime RAM. safe_exp() emits a correctly escaped C string literal.
            cg.add(
                var.add_embedded(
                    entry[CONF_ID], entry[CONF_TYPE], content, len(content.encode("utf-8"))
                )
            )
        else:
            cg.add(var.add_storage(entry[CONF_ID], entry[CONF_TYPE], entry[CONF_PATH]))
