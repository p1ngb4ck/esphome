import hashlib
import logging
from pathlib import Path
from tempfile import TemporaryDirectory
import time
from typing import Any

import voluptuous as vol

from esphome import external_files
import esphome.codegen as cg
from esphome.components import esp32, update
from esphome.components.const import CONF_SHA256
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PATH, CONF_SOURCE, CONF_TYPE, CONF_URL
from esphome.core import CORE, ID, HexInt
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

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
UpdateMethod = esp32_hosted_ns.enum("UpdateMethod", is_class=True)
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

EMBEDDED_SCHEMA = cv.All(
    BASE_SCHEMA.extend(
        {
            cv.Optional(CONF_PATH): cv.file_,
            cv.Optional(CONF_URL): cv.All(cv.url, vol.Match(r"^https?://")),
            cv.Required(CONF_SHA256): _validate_sha256,
        }
    ),
    cv.has_exactly_one_key(CONF_PATH, CONF_URL),
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
def _validate_storage_source(config: ConfigType) -> ConfigType:
    if not config[CONF_SOURCE].lower().endswith(".json") and CONF_SHA256 not in config:
        raise cv.Invalid("sha256 is required unless source is a manifest (.json)")
    return config


STORAGE_SCHEMA = cv.All(
    BASE_SCHEMA.extend(
        {
            cv.Required(CONF_SOURCE): cv.string_strict,
            cv.Optional(CONF_SHA256): _validate_sha256,
        }
    ),
    _validate_storage_source,
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


# The other build's rename finishes within milliseconds; a few short checks cover it
CACHE_RACE_RETRIES = 5
CACHE_RACE_RETRY_S = 0.05


def _check_firmware_hash(data: bytes, expected: str, source: str | Path) -> None:
    calculated = hashlib.sha256(data).hexdigest()
    if calculated != expected:
        raise cv.Invalid(
            f"SHA256 mismatch for {source}: expected {expected}, got {calculated}"
        )


def _cached_image_digest(target: Path) -> str | None:
    """The SHA256 of the cached image, or None while it is missing or another build still writes it."""
    try:
        return hashlib.sha256(target.read_bytes()).hexdigest()
    except OSError:
        return None


def _firmware_path(config: ConfigType) -> Path:
    """The configured image file, or the cache slot of the pinned digest for a downloaded one."""
    if CONF_URL in config:
        cache_dir = external_files.compute_local_file_dir("esp32_hosted") / "firmware"
        return cache_dir / f"{config[CONF_SHA256].lower()}.bin"
    return CORE.relative_config_path(config[CONF_PATH])


def _validate_firmware(config: ConfigType) -> None:
    """Fetch and verify the image here; codegen only reads the file this leaves behind."""
    if config[CONF_TYPE] != TYPE_EMBEDDED:
        return
    expected = config[CONF_SHA256].lower()
    target = _firmware_path(config)
    if (url := config.get(CONF_URL)) is None:
        _check_firmware_hash(target.read_bytes(), expected, target)
        return
    # A pinned digest needs no remote freshness check.
    if (actual := _cached_image_digest(target)) == expected:
        return
    if actual is not None:
        # Named after its digest, so a mismatch means the file was damaged or edited
        _LOGGER.warning(
            "Cached firmware %s hashes to %s, downloading it again", target, actual
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    # Keep unverified bytes out of the shared cache, even across concurrent builds.
    with TemporaryDirectory(dir=target.parent) as temporary:
        candidate = Path(temporary) / "firmware.bin"
        data = external_files.download_content(url, candidate, allow_stale=False)
        _check_firmware_hash(data, expected, url)
        for attempt in range(CACHE_RACE_RETRIES + 1):
            try:
                candidate.replace(target)
                return
            except OSError as err:
                # Windows refuses to replace a file another build holds at that moment; retry, or
                # take that build's copy: named after its digest, a verified one is the same image
                if _cached_image_digest(target) == expected:
                    return
                if attempt == CACHE_RACE_RETRIES:
                    raise cv.Invalid(
                        f"Could not cache the firmware from {url}: {err}"
                    ) from err
                time.sleep(CACHE_RACE_RETRY_S)


FINAL_VALIDATE_SCHEMA = _validate_firmware


async def to_code(config: dict[str, Any]) -> None:
    var = await update.new_update(config)

    firmware_type = config[CONF_TYPE]
    if firmware_type == TYPE_EMBEDDED:
        firmware_data = _firmware_path(config).read_bytes()
        rhs = [HexInt(x) for x in firmware_data]
        arr_id = ID(f"{config[CONF_ID]}_data", is_declaration=True, type=cg.uint8)
        prog_arr = cg.progmem_array(arr_id, rhs)

        sha256_bytes = bytes.fromhex(config[CONF_SHA256])
        cg.add(var.set_firmware_sha256([HexInt(b) for b in sha256_bytes]))
        cg.add(var.set_firmware_data(prog_arr))
        cg.add(var.set_firmware_size(len(firmware_data)))
        cg.add(var.set_method(UpdateMethod.EMBEDDED))
        cg.add_define("USE_ESP32_HOSTED_EMBEDDED_UPDATE")
    elif firmware_type == TYPE_STORAGE:
        # Read the firmware from a mounted storage at runtime. The path is resolved through the
        # storage registry when the update is performed; the sha256 is verified while streaming.
        from esphome.components.storage import request_storage_worker

        if CONF_SHA256 in config:
            sha256_bytes = bytes.fromhex(config[CONF_SHA256])
            cg.add(var.set_firmware_sha256([HexInt(b) for b in sha256_bytes]))
        cg.add(var.set_method(UpdateMethod.STORAGE))
        cg.add(var.set_storage_path(config[CONF_SOURCE]))
        cg.add_define("USE_ESP32_HOSTED_STORAGE_UPDATE")
        request_storage_worker()
    else:
        await cg.add_variable(var.set_http_request_parent, config[CONF_HTTP_REQUEST_ID])
        cg.add(var.set_source_url(config[CONF_SOURCE]))
        cg.add(var.set_method(UpdateMethod.HTTP))
        cg.add_define("USE_ESP32_HOSTED_HTTP_UPDATE")

    await cg.register_component(var, config)
