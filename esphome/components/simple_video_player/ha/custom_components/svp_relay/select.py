"""Channel choice for the panel configured as panel_host."""

from __future__ import annotations

from datetime import timedelta
import logging

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.typing import ConfigType, DiscoveryInfoType

from . import CONF_PANEL_HOST, CONF_PANEL_PORT, DOMAIN
from .relay import SatRelay

_LOGGER = logging.getLogger(__name__)

SCAN_INTERVAL = timedelta(minutes=10)


async def async_setup_platform(
    hass: HomeAssistant,
    config: ConfigType,
    async_add_entities: AddEntitiesCallback,
    discovery_info: DiscoveryInfoType | None = None,
) -> None:
    async_add_entities([ChannelSelect(hass.data[DOMAIN])], update_before_add=True)


class ChannelSelect(SelectEntity):
    _attr_name = "SVP Sat channel"
    _attr_unique_id = "svp_relay_channel"
    _attr_icon = "mdi:satellite-variant"

    def __init__(self, relay: SatRelay) -> None:
        self._relay = relay
        self._refs: dict[str, str] = {}
        self._attr_options = []
        self._attr_current_option = None

    async def async_update(self) -> None:
        try:
            channels = await self._relay.async_channels()
        except Exception as err:  # noqa: BLE001
            _LOGGER.warning("Channel list from the receiver failed: %s", err)
            return
        self._refs = {c["name"]: c["ref"] for c in channels}
        self._attr_options = list(self._refs)

    async def async_select_option(self, option: str) -> None:
        conf = self._relay.conf
        await self._relay.async_play(self._refs[option], conf[CONF_PANEL_HOST], conf[CONF_PANEL_PORT])
        self._attr_current_option = option
        self.async_write_ha_state()
