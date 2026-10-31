"""Bluedroid pairing data (and GATT cache) kept in PSRAM and saved to a file, or left in NVS."""

import hashlib
from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    get_esp32_variant,
    idf_version,
)
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PATH, CONF_TRIGGER_ID
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]

CONF_BACKEND = "backend"
CONF_GATT_CACHE = "gatt_cache"
CONF_WRITE_DELAY = "write_delay"
CONF_STORAGE_TIMEOUT = "storage_timeout"
CONF_ON_READY = "on_ready"
CONF_ON_ERROR = "on_error"

BACKEND_STORAGE = "storage"
BACKEND_NVS = "nvs"

bt_persistence_ns = cg.esphome_ns.namespace("bt_persistence")
BtPersistence = bt_persistence_ns.class_("BtPersistence", cg.Component)
ReadyTrigger = bt_persistence_ns.class_("ReadyTrigger", automation.Trigger.template())
ErrorTrigger = bt_persistence_ns.class_("ErrorTrigger", automation.Trigger.template())
SetPathAction = bt_persistence_ns.class_("SetPathAction", automation.Action)


def _vfs_path(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/") or value.count("/") < 2:
        raise cv.Invalid(
            "path must be a file on a storage, e.g. /sdcard/bluetooth/bt_store.bin"
        )
    if len(value) >= 192:
        raise cv.Invalid("path must be shorter than 192 characters")
    return value


_COMMON = {
    # Persist the GATT client cache (CONFIG_BT_GATTC_CACHE_NVS_FLASH); unset leaves it as is.
    cv.Optional(CONF_GATT_CACHE): cv.boolean,
}

CONFIG_SCHEMA = cv.typed_schema(
    {
        BACKEND_STORAGE: cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(BtPersistence),
                cv.Required(CONF_PATH): _vfs_path,
                cv.Optional(
                    CONF_WRITE_DELAY, default="2s"
                ): cv.positive_time_period_milliseconds,
                cv.Optional(
                    CONF_STORAGE_TIMEOUT, default="20s"
                ): cv.positive_time_period_milliseconds,
                cv.Optional(CONF_ON_READY): automation.validate_automation(
                    {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReadyTrigger)}
                ),
                cv.Optional(CONF_ON_ERROR): automation.validate_automation(
                    {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ErrorTrigger)}
                ),
                **_COMMON,
            }
        ).extend(cv.COMPONENT_SCHEMA),
        BACKEND_NVS: cv.Schema(_COMMON),
    },
    key=CONF_BACKEND,
    default_type=BACKEND_STORAGE,
    lower=True,
)


def _final_validate(config: ConfigType) -> ConfigType:
    full = fv.full_config.get()
    if "esp32_ble" not in full and "usb_bluetooth" not in full:
        raise cv.Invalid(
            "bt_persistence needs Bluedroid: configure esp32_ble or usb_bluetooth"
        )
    if config[CONF_BACKEND] == BACKEND_STORAGE and "storage" not in full:
        raise cv.Invalid("backend 'storage' needs a storage component")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


_MARKER = ".esphome_bt_persistence"
_IDF_DIR = Path(__file__).parent / "idf"
_STORE_SOURCE = "esphome_nvs_store.c"
_STORE_HEADERS = ("esphome_nvs_store.h", "esphome_nvs_redirect.h")
_REDIRECT_INCLUDE = '#include "osi/esphome_nvs_redirect.h"\n'
_NVS_INCLUDES = ('#include "nvs.h"\n', '#include "nvs_flash.h"\n')
_PATCHED_SOURCES = (
    "common/osi/config.c",
    "host/bluedroid/bta/gatt/bta_gattc_co.c",
)
_CMAKE_LIST = "common/CMakeLists.txt"
_CMAKE_ANCHOR = '"${CMAKE_CURRENT_LIST_DIR}/osi/config.c"'


def _sync_bt_override() -> str:
    """Copy the IDF bt component, point its NVS calls at the ESPHome store, return the copy.

    Same scheme as the exFAT FatFs override: the copy sits in <build>/components-src/bt, outside
    every component search path, and replaces the built-in bt only through the override_path the
    caller writes into the manifest. Stamped with the IDF version and the store sources.
    """
    import shutil

    from esphome.espidf.framework import _get_framework_path, check_esp_idf_install

    idf_ver = str(idf_version())
    digest = hashlib.sha256()
    for name in (_STORE_SOURCE, *_STORE_HEADERS):
        digest.update((_IDF_DIR / name).read_bytes())
    stamp = f"v1:{idf_ver}:{digest.hexdigest()}"
    dest = Path(CORE.build_path) / "components-src" / "bt"
    marker = dest / _MARKER
    if marker.is_file() and marker.read_text() == stamp:
        return str(dest)

    src = _get_framework_path(idf_ver) / "components" / "bt"
    if not src.is_dir():
        check_esp_idf_install(idf_ver, targets=[get_esp32_variant()])
    if not src.is_dir():
        raise cv.Invalid(
            f"bt_persistence: cannot locate the ESP-IDF bt component (looked in {src})"
        )
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns("test_apps", ".git"))

    for rel in _PATCHED_SOURCES:
        target = dest / rel
        text = target.read_text() if target.is_file() else ""
        positions = [text.find(line) for line in _NVS_INCLUDES]
        if any(text.count(line) != 1 for line in _NVS_INCLUDES):
            raise cv.Invalid(
                f"bt_persistence: unexpected NVS includes in {rel} of IDF {idf_ver}"
            )
        last = max(positions)
        line = _NVS_INCLUDES[positions.index(last)]
        cut = last + len(line)
        target.write_text(text[:cut] + _REDIRECT_INCLUDE + text[cut:])

    cmake = dest / _CMAKE_LIST
    text = cmake.read_text()
    if text.count(_CMAKE_ANCHOR) != 1:
        raise cv.Invalid(
            f"bt_persistence: unexpected source list in {_CMAKE_LIST} of IDF {idf_ver}"
        )
    text = text.replace(
        _CMAKE_ANCHOR,
        f'{_CMAKE_ANCHOR}\n    "${{CMAKE_CURRENT_LIST_DIR}}/osi/{_STORE_SOURCE}"',
        1,
    )
    cmake.write_text(text)

    shutil.copyfile(_IDF_DIR / _STORE_SOURCE, dest / "common" / "osi" / _STORE_SOURCE)
    for name in _STORE_HEADERS:
        shutil.copyfile(
            _IDF_DIR / name, dest / "common" / "osi" / "include" / "osi" / name
        )
    marker.write_text(stamp)
    return str(dest)


async def to_code(config: ConfigType) -> None:
    if (gatt_cache := config.get(CONF_GATT_CACHE)) is not None:
        add_idf_sdkconfig_option("CONFIG_BT_GATTC_CACHE_NVS_FLASH", gatt_cache)
    if config[CONF_BACKEND] != BACKEND_STORAGE:
        return

    add_idf_component(name="bt", override_path=_sync_bt_override())
    request_storage_worker()
    cg.add_define("USE_BT_PERSISTENCE")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_default_path(config[CONF_PATH]))
    cg.add(var.set_write_delay(config[CONF_WRITE_DELAY]))
    cg.add(var.set_storage_timeout(config[CONF_STORAGE_TIMEOUT]))
    for conf in config.get(CONF_ON_READY, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


@automation.register_action(
    "bt_persistence.set_path",
    SetPathAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(BtPersistence),
            cv.Required(CONF_PATH): cv.templatable(_vfs_path),
        }
    ),
    synchronous=True,
)
async def set_path_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(template_))
    return var
