"""SVP Sat Relay: Enigma2 channels on a simple_video_player panel.

The panel fetches the channel list from here and asks for a channel; this
relays that channel from the receiver to the panel's stream_port.
"""

from __future__ import annotations

import logging

from aiohttp import web
import voluptuous as vol

from homeassistant.components.http import HomeAssistantView
from homeassistant.const import EVENT_HOMEASSISTANT_STOP, Platform
from homeassistant.core import Event, HomeAssistant
from homeassistant.helpers import config_validation as cv, discovery
from homeassistant.helpers.typing import ConfigType

from .relay import SatRelay

_LOGGER = logging.getLogger(__name__)

DOMAIN = "svp_relay"

CONF_RECEIVER = "receiver"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_STREAM_PORT = "stream_port"
CONF_BOUQUET = "bouquet"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"
CONF_ROTATE = "rotate"
CONF_FPS = "fps"
CONF_QUALITY = "quality"
CONF_FFMPEG = "ffmpeg"
CONF_TAP_TO_STOP = "tap_to_stop"
CONF_PANEL_HOST = "panel_host"
CONF_PANEL_PORT = "panel_port"

CONFIG_SCHEMA = vol.Schema(
    {
        DOMAIN: vol.Schema(
            {
                vol.Required(CONF_RECEIVER): cv.url,
                vol.Optional(CONF_USERNAME): cv.string,
                vol.Optional(CONF_PASSWORD): cv.string,
                vol.Optional(CONF_STREAM_PORT, default=8001): cv.port,
                vol.Optional(CONF_BOUQUET): cv.string,
                vol.Optional(CONF_WIDTH, default=800): cv.positive_int,
                vol.Optional(CONF_HEIGHT, default=1280): cv.positive_int,
                vol.Optional(CONF_ROTATE, default=90): vol.In([0, 90, 180, 270]),
                vol.Optional(CONF_FPS, default=25): vol.All(vol.Coerce(int), vol.Range(min=1, max=60)),
                vol.Optional(CONF_QUALITY, default=7): vol.All(vol.Coerce(int), vol.Range(min=2, max=31)),
                vol.Optional(CONF_FFMPEG, default="ffmpeg"): cv.string,
                vol.Optional(CONF_TAP_TO_STOP, default=True): cv.boolean,
                vol.Optional(CONF_PANEL_HOST): cv.string,
                vol.Optional(CONF_PANEL_PORT, default=5000): cv.port,
            }
        )
    },
    extra=vol.ALLOW_EXTRA,
)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    conf = config[DOMAIN]
    relay = SatRelay(hass, conf)
    hass.data[DOMAIN] = relay

    hass.http.register_view(ChannelsView(relay))
    hass.http.register_view(PlayView(relay))
    hass.http.register_view(StopView(relay))

    if conf.get(CONF_PANEL_HOST):
        for platform in (Platform.SELECT, Platform.BUTTON):
            hass.async_create_task(discovery.async_load_platform(hass, platform, DOMAIN, {}, config))

    async def _stop(_: Event) -> None:
        await relay.async_stop()

    hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STOP, _stop)
    return True


class ChannelsView(HomeAssistantView):
    """GET /api/svp_relay/channels -> {"channels": [{"name": ..., "ref": ...}]}"""

    url = "/api/svp_relay/channels"
    name = "api:svp_relay:channels"

    def __init__(self, relay: SatRelay) -> None:
        self._relay = relay

    async def get(self, request: web.Request) -> web.Response:
        try:
            channels = await self._relay.async_channels(force=request.query.get("refresh") == "1")
        except Exception as err:  # noqa: BLE001 -- the receiver's error goes back to the panel
            _LOGGER.warning("Channel list from the receiver failed: %s", err)
            return self.json_message(f"receiver: {err}", 502)
        return self.json({"channels": channels})


class PlayView(HomeAssistantView):
    """POST /api/svp_relay/play {"ref": ..., "port": 5000[, "host": ...]} -> relay to the caller."""

    url = "/api/svp_relay/play"
    name = "api:svp_relay:play"

    def __init__(self, relay: SatRelay) -> None:
        self._relay = relay

    async def post(self, request: web.Request) -> web.Response:
        try:
            data = await request.json()
            ref = str(data["ref"])
            port = int(data.get("port", 5000))
        except (ValueError, KeyError, TypeError):
            return self.json_message("expected {\"ref\": ..., \"port\": ...}", 400)
        host = str(data.get("host") or request.remote)
        try:
            await self._relay.async_play(ref, host, port)
        except Exception as err:  # noqa: BLE001
            _LOGGER.warning("Relay to %s:%d failed: %s", host, port, err)
            return self.json_message(f"relay: {err}", 502)
        return self.json({"ok": True})


class StopView(HomeAssistantView):
    """POST /api/svp_relay/stop"""

    url = "/api/svp_relay/stop"
    name = "api:svp_relay:stop"

    def __init__(self, relay: SatRelay) -> None:
        self._relay = relay

    async def post(self, request: web.Request) -> web.Response:
        await self._relay.async_stop()
        return self.json({"ok": True})
