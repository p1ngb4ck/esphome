import esphome.config_validation as cv
from esphome.core import CORE

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

__all__ = [
    "ADDRESS_GLOBAL_SCHEMA",
    "CONF_ADDRESS_GLOBAL",
    "CONF_BT_HOST_ID",
    "BTHost",
    "bt_host_ns",
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
        }
    ),
    _default_pairing,
)


def pairing_configured() -> bool:
    raw = (CORE.raw_config or {}).get("bt_host") or {}
    return CONF_PAIRING in raw or any(c in CORE.loaded_integrations for c in PAIRING_DRIVERS)


def _final_validate(config):
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
