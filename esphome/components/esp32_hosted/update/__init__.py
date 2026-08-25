import hashlib
from typing import Any

import esphome.codegen as cg
from esphome.components import esp32, update
from esphome.components.const import CONF_SHA256
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PATH, CONF_SOURCE, CONF_TYPE
from esphome.core import CORE, ID, HexInt
from esphome.types import ConfigType

CODEOWNERS = ["@swoboda1337"]
DEPENDENCIES = ["esp32_hosted"]

CONF_HTTP_REQUEST_ID = "http_request_id"

TYPE_EMBEDDED = "embedded"
TYPE_HTTP = "http"
TYPE_STORAGE = "storage"


def AUTO_LOAD(config: ConfigType) -> list[str]:
    """sha256/watchdog/json are always needed; the storage firmware source additionally needs
    the storage component, pulled in only when 'type: storage' is selected — other sources pay
    nothing for it."""
    base = ["sha256", "watchdog", "json"]
    if config and config.get(CONF_TYPE) == TYPE_STORAGE:
        return base + ["storage"]
    return base

esp32_hosted_ns = cg.esphome_ns.namespace("esp32_hosted")
http_request_ns = cg.esphome_ns.namespace("http_request")
HttpRequestComponent = http_request_ns.class_("HttpRequestComponent", cg.Component)
Esp32HostedUpdate = esp32_hosted_ns.class_(
    "Esp32HostedUpdate", update.UpdateEntity, cg.PollingComponent
)


def _validate_sha256(value: Any) -> str:
    value = cv.string_strict(value)
    if len(value) != 64:
        raise cv.Invalid("SHA256 must be 64 hexadecimal characters")
    try:
        bytes.fromhex(value)
    except ValueError as e:
        raise cv.Invalid(f"SHA256 must be valid hexadecimal: {e}") from e
    return value


BASE_SCHEMA = update.update_schema(Esp32HostedUpdate, device_class="firmware").extend(
    cv.polling_component_schema("6h")
)

EMBEDDED_SCHEMA = BASE_SCHEMA.extend(
    {
        cv.Required(CONF_PATH): cv.file_,
        cv.Required(CONF_SHA256): _validate_sha256,
    }
)

HTTP_SCHEMA = BASE_SCHEMA.extend(
    {
        cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
        cv.Required(CONF_SOURCE): cv.url,
    }
)

# Read the co-processor firmware from a mounted storage at runtime. CONF_SOURCE is a plain
# storage path (e.g. /sdcard/slave_fw.bin) resolved through the storage registry — not a URL and
# not a build-time file, so it is validated as a string and never opened during config.
STORAGE_SCHEMA = BASE_SCHEMA.extend(
    {
        cv.Required(CONF_SOURCE): cv.string_strict,
        cv.Required(CONF_SHA256): _validate_sha256,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            TYPE_EMBEDDED: EMBEDDED_SCHEMA,
            TYPE_HTTP: HTTP_SCHEMA,
            TYPE_STORAGE: STORAGE_SCHEMA,
        }
    ),
    esp32.only_on_variant(
        supported=[
            esp32.VARIANT_ESP32H2,
            esp32.VARIANT_ESP32P4,
        ]
    ),
)


def _validate_firmware(config: dict[str, Any]) -> None:
    if config[CONF_TYPE] != TYPE_EMBEDDED:
        return

    path = CORE.relative_config_path(config[CONF_PATH])
    with path.open("rb") as f:
        firmware_data = f.read()
    calculated = hashlib.sha256(firmware_data).hexdigest()
    expected = config[CONF_SHA256].lower()
    if calculated != expected:
        raise cv.Invalid(
            f"SHA256 mismatch for {config[CONF_PATH]}: expected {expected}, got {calculated}"
        )


FINAL_VALIDATE_SCHEMA = _validate_firmware


async def to_code(config: dict[str, Any]) -> None:
    var = await update.new_update(config)

    firmware_type = config[CONF_TYPE]
    if firmware_type == TYPE_EMBEDDED:
        path = config[CONF_PATH]
        with CORE.relative_config_path(path).open("rb") as f:
            firmware_data = f.read()
        rhs = [HexInt(x) for x in firmware_data]
        arr_id = ID(f"{config[CONF_ID]}_data", is_declaration=True, type=cg.uint8)
        prog_arr = cg.progmem_array(arr_id, rhs)

        sha256_bytes = bytes.fromhex(config[CONF_SHA256])
        cg.add(var.set_firmware_sha256([HexInt(b) for b in sha256_bytes]))
        cg.add(var.set_firmware_data(prog_arr))
        cg.add(var.set_firmware_size(len(firmware_data)))
    elif firmware_type == TYPE_STORAGE:
        # Read the firmware from a mounted storage at runtime. The path is resolved through the
        # storage registry when the update is performed; the sha256 is verified while streaming.
        sha256_bytes = bytes.fromhex(config[CONF_SHA256])
        cg.add(var.set_firmware_sha256([HexInt(b) for b in sha256_bytes]))
        cg.add(var.set_storage_path(config[CONF_SOURCE]))
        cg.add_define("USE_ESP32_HOSTED_STORAGE_UPDATE")
    else:
        http_request_var = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
        cg.add(var.set_http_request_parent(http_request_var))
        cg.add(var.set_source_url(config[CONF_SOURCE]))
        cg.add_define("USE_ESP32_HOSTED_HTTP_UPDATE")

    await cg.register_component(var, config)
