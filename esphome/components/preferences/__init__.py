from esphome import preferences
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority
from esphome.types import ConfigType

CODEOWNERS = ["@esphome/core"]

preferences_ns = cg.esphome_ns.namespace("preferences")
esp32_ns = cg.esphome_ns.namespace("esp32")
binary_storage_ns = cg.esphome_ns.namespace("binary_storage")
NVSStore = binary_storage_ns.class_("NVSStore")
IntervalSyncer = preferences_ns.class_("IntervalSyncer", cg.Component)

CONF_FLASH_WRITE_INTERVAL = "flash_write_interval"
CONF_RTC_STORAGE = "rtc_storage"
CONF_STORAGE_BACKEND = "storage_backend"
CONF_EXTERNAL_NVS = "external_nvs"
CONF_KEEP_EARLY = "keep_early"
CONF_USE_INTERNAL_NVS = "use_internal_nvs"


def AUTO_LOAD(config: ConfigType) -> list[str]:
    # Route the flash preferences through the storage KeyValueStorage interface + NVS backend, but
    # only pull those components into the build when the option is actually enabled.
    if config and (config.get(CONF_STORAGE_BACKEND) or config.get(CONF_EXTERNAL_NVS)):
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
        # esp32 only: route the flash preference path to an external esp_partition NVS store
        # (a `format: nvs` region on external flash). Flash-backed preferences created after the
        # external store is bound live there; the RTC path (in_flash=false) is unaffected. The
        # referenced id is the NVS region's id.
        cv.Optional(CONF_EXTERNAL_NVS): cv.All(cv.use_id(NVSStore), cv.only_on_esp32),
        # esp32 only: components that read preferences before the storage stage (and so cannot use
        # the external store) -- their preferences are kept early (RTC). Validation of size/timing
        # is handled separately; listing here marks the intended exceptions.
        cv.Optional(CONF_KEEP_EARLY): cv.All(
            cv.ensure_list(cv.use_id(cg.Component)), cv.only_on_esp32
        ),
        # esp32 only: keep the internal system "esphome" NVS as a preferences backend. Set false
        # only with external_nvs and when every pre-storage reader uses RTC (keep_early) -- the
        # internal namespace is then not opened and preferences read before the external store is
        # bound return defaults. nvs_flash_init() still runs (IDF wifi/BT may need it).
        cv.Optional(CONF_USE_INTERNAL_NVS, default=True): cv.All(
            cv.boolean, cv.only_on_esp32
        ),
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
    if config.get(CONF_KEEP_EARLY) and config.get(CONF_EXTERNAL_NVS) is None:
        raise cv.Invalid(
            f"'{CONF_KEEP_EARLY}' only applies together with '{CONF_EXTERNAL_NVS}': it marks the "
            f"components that must NOT use the external store (they read before it is available). "
            f"Those components route their preferences to RTC via their own in_flash/store option; "
            f"this list is the validated intent, not an automatic router."
        )
    if not config.get(CONF_USE_INTERNAL_NVS, True):
        if config.get(CONF_EXTERNAL_NVS) is None:
            raise cv.Invalid(
                f"'{CONF_USE_INTERNAL_NVS}: false' needs '{CONF_EXTERNAL_NVS}' -- without an "
                f"external store there is no flash preferences backend left"
            )
        cg.add_define("USE_ESP32_PREFERENCES_NO_INTERNAL")
    if (external := config.get(CONF_EXTERNAL_NVS)) is not None:
        # Route flash preferences through the external esp_partition NVS store. Reuses the same
        # KeyValueStorage seam as storage_backend, so save/load/sync are unchanged.
        cg.add_define("USE_ESP32_PREFERENCES_STORAGE")
        CORE.data.setdefault("binary_storage_device_types", set()).add("nvs")
        store = await cg.get_variable(external)
        cg.add(esp32_ns.set_external_preferences_store(store))
    await cg.register_component(var, config)
