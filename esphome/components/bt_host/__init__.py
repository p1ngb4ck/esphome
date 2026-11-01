from esphome.components.esp32 import get_esp32_variant
from esphome.components.esp32.const import (
    VARIANT_ESP32,
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32S31,
)
import esphome.config_validation as cv
from esphome.core import CORE
import esphome.final_validate as fv

from . import classic, pairing, persistence
from .pairing import (
    ADDRESS_GLOBAL_SCHEMA,
    CONF_ADDRESS_GLOBAL,
    CONF_BT_HOST_ID,
    BTHost,
    bt_host_ns,
    setup_address_global,
    validate_address_global,
)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]

CONF_PAIRING = "pairing"
CONF_PERSISTENCE = "persistence"
CONF_BLE_CONTROLLER = "ble_controller"
CONF_CLASSIC_CONTROLLER = "classic_controller"
CONTROLLERS = ("auto", "onchip", "usb", "c6", "c6_offload")
ONCHIP_BLE = (VARIANT_ESP32, VARIANT_ESP32S3, VARIANT_ESP32S31)
ONCHIP_CLASSIC = (VARIANT_ESP32, VARIANT_ESP32S31)
USB_HOST_VARIANTS = (VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32S31, VARIANT_ESP32P4)

__all__ = [
    "ADDRESS_GLOBAL_SCHEMA",
    "CONF_ADDRESS_GLOBAL",
    "CONF_BT_HOST_ID",
    "BTHost",
    "bt_host_ns",
    "classic_controller",
    "classic_in_use",
    "pairing_configured",
    "setup_address_global",
    "validate_address_global",
]


def AUTO_LOAD(config=None) -> list[str]:
    pairing_config = (config or {}).get(CONF_PAIRING)
    if pairing_config is None:
        return []
    return [c for c in pairing.AUTO_LOAD(pairing_config) if c != "bt_host"]


PAIRING_DRIVERS = ("bt_hid", "ble_client_hid", "ble_broadcast")


def _default_pairing(config):
    if CONF_PAIRING not in config and any(c in CORE.loaded_integrations for c in PAIRING_DRIVERS):
        config[CONF_PAIRING] = pairing.CONFIG_SCHEMA({})
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_PAIRING): pairing.CONFIG_SCHEMA,
            cv.Optional(CONF_PERSISTENCE): persistence.CONFIG_SCHEMA,
            cv.Optional(CONF_BLE_CONTROLLER, default="auto"): cv.one_of(*CONTROLLERS, lower=True),
            cv.Optional(CONF_CLASSIC_CONTROLLER, default="auto"): cv.one_of(*CONTROLLERS, lower=True),
        }
    ),
    _default_pairing,
)


def pairing_configured() -> bool:
    raw = (CORE.raw_config or {}).get("bt_host") or {}
    return CONF_PAIRING in raw or any(c in CORE.loaded_integrations for c in PAIRING_DRIVERS)


def _dongles(full_config, key) -> bool:
    return any(c.get(key) for c in full_config.get("usb_bluetooth") or [])


def classic_controller(full_config) -> str | None:
    if _dongles(full_config, "bluedroid"):
        return "usb"
    if get_esp32_variant() in ONCHIP_CLASSIC:
        return "onchip"
    return None


def ble_controller(full_config) -> str | None:
    if _dongles(full_config, "c6_offload"):
        return "c6_offload"
    if "esp32_ble" in full_config:
        if get_esp32_variant() == VARIANT_ESP32P4:
            return "c6"
        return "onchip"
    if _dongles(full_config, "bluedroid"):
        return "usb"
    return None


_NEEDS = {
    "onchip": "a chip with that radio role",
    "usb": "usb_bluetooth with bluedroid: true",
    "c6": "esp32_ble on the ESP32-P4 (the C6 over esp32_hosted)",
    "c6_offload": "usb_bluetooth with c6_offload: true and bt_offload",
}


def _check_controller(config, key, found, possible) -> None:
    wanted = config[key]
    if wanted == "auto":
        return
    if wanted not in possible:
        raise cv.Invalid(f"{key} {wanted} is not possible on this chip (possible: {', '.join(possible)})")
    if found != wanted:
        raise cv.Invalid(f"{key} {wanted} needs {_NEEDS[wanted]} (configured now: {found or 'none'})")


def _possible(role) -> tuple[str, ...]:
    variant = get_esp32_variant()
    options = []
    if variant in (ONCHIP_BLE if role == "ble" else ONCHIP_CLASSIC):
        options.append("onchip")
    if variant in USB_HOST_VARIANTS:
        options.append("usb")
    if variant == VARIANT_ESP32P4:
        options += ["c6"] if role == "ble" else []
        options.append("c6_offload")
    return tuple(options)


def _final_validate(config):
    full = fv.full_config.get()
    _check_controller(config, CONF_BLE_CONTROLLER, ble_controller(full), _possible("ble"))
    _check_controller(config, CONF_CLASSIC_CONTROLLER, classic_controller(full), _possible("classic"))
    if CONF_PAIRING in config:
        pairing.FINAL_VALIDATE_SCHEMA(config[CONF_PAIRING])
    if CONF_PERSISTENCE in config:
        persistence.FINAL_VALIDATE_SCHEMA(config[CONF_PERSISTENCE])
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def classic_in_use(full_config) -> bool:
    slots = ((full_config.get("bt_host") or {}).get(CONF_PAIRING) or {}).get("slots") or {}
    return bool(slots.get("bt_hid")) or any(key in full_config for key in ("bt_hid", "a2dp_source"))


async def to_code(config):
    if classic_in_use(CORE.config):
        await classic.to_code({})
    if CONF_PAIRING in config:
        await pairing.to_code(config[CONF_PAIRING])
    if CONF_PERSISTENCE in config:
        await persistence.to_code(config[CONF_PERSISTENCE])
