"""Stop whatever is being relayed."""

from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.typing import ConfigType, DiscoveryInfoType

from . import DOMAIN
from .relay import SatRelay


async def async_setup_platform(
    hass: HomeAssistant,
    config: ConfigType,
    async_add_entities: AddEntitiesCallback,
    discovery_info: DiscoveryInfoType | None = None,
) -> None:
    async_add_entities([StopButton(hass.data[DOMAIN])])


class StopButton(ButtonEntity):
    _attr_name = "SVP Sat stop"
    _attr_unique_id = "svp_relay_stop"
    _attr_icon = "mdi:stop"

    def __init__(self, relay: SatRelay) -> None:
        self._relay = relay

    async def async_press(self) -> None:
        await self._relay.async_stop()
