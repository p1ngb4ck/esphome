from esphome import preferences
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority
from esphome.types import ConfigType

CODEOWNERS = ["@esphome/core"]

preferences_ns = cg.esphome_ns.namespace("preferences")
IntervalSyncer = preferences_ns.class_("IntervalSyncer", cg.Component)

CONF_FLASH_WRITE_INTERVAL = "flash_write_interval"
CONF_RTC_STORAGE = "rtc_storage"
CONF_STORAGE_BACKEND = "storage_backend"


def AUTO_LOAD(config: ConfigType) -> list[str]:
    # Route the flash preferences through the storage KeyValueStorage interface + NVS backend, but
    # only pull those components into the build when the option is actually enabled.
    if config and config.get(CONF_STORAGE_BACKEND):
        return ["storage", "binary_storage"]
    return []


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(IntervalSyncer),
        cv.Optional(CONF_FLASH_WRITE_INTERVAL, default="60s"): cv.update_interval,
        # Compile the RTC-backed storage into the ESP32 preferences backend even
        # when no other option selects it, so components (including external
        # ones) requesting in_flash=false are honoured instead of falling back
        # to NVS. No default: absence means "no request" (see
        # preferences.validate_rtc_storage for the per-platform rules).
        cv.Optional(CONF_RTC_STORAGE): preferences.validate_rtc_storage,
        # esp32 only: route the flash (NVS) preference path through the storage
        # KeyValueStorage interface instead of raw nvs_* calls. Format-identical
        # (same system "esphome" namespace); the RTC path is unaffected.
        cv.Optional(CONF_STORAGE_BACKEND): cv.All(cv.boolean, cv.only_on_esp32),
    }
).extend(cv.COMPONENT_SCHEMA)


@coroutine_with_priority(CoroPriority.PREFERENCES)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    write_interval = config[CONF_FLASH_WRITE_INTERVAL]
    if write_interval.total_milliseconds == 0:
        cg.add_define("USE_PREFERENCES_SYNC_EVERY_LOOP")
    else:
        cg.add(var.set_write_interval(write_interval))
    if config.get(CONF_RTC_STORAGE):
        preferences.request_rtc_storage()
    if config.get(CONF_STORAGE_BACKEND):
        cg.add_define("USE_ESP32_PREFERENCES_STORAGE")
        # FILTER_SOURCE_FILES in binary_storage is exclude-based: keep nvs_store.cpp
        # compiled even though no `type: nvs` device is configured.
        CORE.data.setdefault("binary_storage_device_types", set()).add("nvs")
    await cg.register_component(var, config)
