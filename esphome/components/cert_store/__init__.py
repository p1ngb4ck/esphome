from pathlib import Path

from esphome.components import esp32
import esphome.codegen as cg
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import CONF_CLIENT_CERTIFICATE, CONF_CLIENT_CERTIFICATE_KEY, CONF_ID
from esphome.core import CORE
from esphome.helpers import cpp_string_escape

CODEOWNERS = ["@p1ngb4ck"]
DOMAIN = "cert_store"

CONF_DIRECTORY = "directory"
CONF_RELAY = "relay"
CONF_HTTP_SOURCES = "http_sources"
CONF_CA = "ca"


def AUTO_LOAD(config=None) -> list[str]:
    if isinstance(config, dict) and (config.get(CONF_RELAY) or config.get(CONF_HTTP_SOURCES)):
        return ["relay_link"]
    return []


cert_store_ns = cg.esphome_ns.namespace("cert_store")
CertStore = cert_store_ns.class_("CertStore", cg.Component)


def _storage_dir(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/"):
        raise cv.Invalid("a directory's storage path, as /sd/certs")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CertStore),
            cv.Optional(CONF_DIRECTORY): _storage_dir,
            cv.Optional(CONF_RELAY, default=False): cv.boolean,
            cv.Optional(CONF_HTTP_SOURCES, default=[]): cv.All(cv.ensure_list(cv.url), cv.Length(max=4)),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_framework(cv.Framework.ESP_IDF),
)


def cert_option(value):
    value = cv.string_strict(value)
    if value.startswith("/"):
        return value
    if ("/" in value or "." in value) and not Path(CORE.relative_config_path(value)).is_file():
        raise cv.Invalid(f"certificate file '{value}' not found relative to the config")
    return value


TLS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_CA): cert_option,
        cv.Optional(CONF_CLIENT_CERTIFICATE): cert_option,
        cv.Optional(CONF_CLIENT_CERTIFICATE_KEY): cert_option,
    }
)


def validate_tls(config):
    if (CONF_CLIENT_CERTIFICATE in config) != (CONF_CLIENT_CERTIFICATE_KEY in config):
        raise cv.Invalid(f"'{CONF_CLIENT_CERTIFICATE}' and '{CONF_CLIENT_CERTIFICATE_KEY}' go together")
    return config


def tls_keys_present(config) -> bool:
    return any(key in config for key in (CONF_CA, CONF_CLIENT_CERTIFICATE, CONF_CLIENT_CERTIFICATE_KEY))


def _ref(value):
    if value is None:
        return "cert_store::CertRef{}"
    if value.startswith("/"):
        return f"cert_store::CertRef{{cert_store::CertSource::STORAGE, {cpp_string_escape(value)}, {len(value)}}}"
    if "/" in value or "." in value:
        content = Path(CORE.relative_config_path(value)).read_text(encoding="utf-8")
        return (
            f"cert_store::CertRef{{cert_store::CertSource::EMBEDDED, {cpp_string_escape(content)}, "
            f"{len(content.encode('utf-8'))}}}"
        )
    return f"cert_store::CertRef{{cert_store::CertSource::NAMED, {cpp_string_escape(value)}, {len(value)}}}"


def tls_refs(config):
    return cg.RawExpression(
        "cert_store::TlsRefs{"
        + ", ".join(_ref(config.get(key)) for key in (CONF_CA, CONF_CLIENT_CERTIFICATE, CONF_CLIENT_CERTIFICATE_KEY))
        + "}"
    )


async def to_code(config):
    var = cg.Pvariable(config[CONF_ID], cg.MockObj(str(CertStore), "::").create())
    await cg.register_component(var, config)
    request_storage_worker()
    cg.add_define("USE_CERT_STORE")
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    esp32.require_mbedtls_sha512()
    if CONF_DIRECTORY in config:
        cg.add(var.set_directory(config[CONF_DIRECTORY]))
    if config[CONF_RELAY] or config[CONF_HTTP_SOURCES]:
        from esphome.components import relay_link

        cg.add_define("USE_CERT_STORE_LINK")
        cg.add(var.set_relay_link(await relay_link.get_relay_link()))
        cg.add(var.set_relay(config[CONF_RELAY]))
        for url in config[CONF_HTTP_SOURCES]:
            cg.add(var.add_http_source(url))
