# svp_relay — Enigma2 channels on a simple_video_player panel

Home Assistant custom integration (works on a plain pip/Core install). It reads the channel list
from an Enigma2 receiver's OpenWebif, and on request relays a channel to the panel: ffmpeg pulls
the receiver's stream, scales/rotates it to the panel, encodes MJPEG + 16-bit PCM and sends it
over udisp to the panel's `stream_port`.

## Install (on the HA host)

```bash
sudo apt install ffmpeg
cp -r custom_components/svp_relay <ha-config>/custom_components/
```

`configuration.yaml`:

```yaml
svp_relay:
  receiver: http://192.168.1.50      # OpenWebif
  # username: root                   # if OpenWebif / streaming auth is on
  # password: !secret enigma_password
  # stream_port: 8001                # receiver streaming port
  # bouquet: "Favourites (TV)"       # only this bouquet
  # width: 800                       # panel native size
  # height: 1280
  # rotate: 90                       # landscape TV -> portrait panel (0/90/180/270)
  # fps: 25
  # quality: 7                       # ffmpeg -q:v
  # tap_to_stop: true                # a tap on the panel ends the relay
  # panel_host: 192.168.1.60         # adds select/button entities for this panel
  # panel_port: 5000
```

Restart HA, then create a long-lived access token (profile page) for the panel.

## HTTP API (Bearer token)

| | |
|---|---|
| `GET /api/svp_relay/channels[?refresh=1]` | `{"channels": [{"name", "ref"}]}` |
| `POST /api/svp_relay/play` `{"ref", "port"[, "host"]}` | relay to the caller's IP (or `host`) |
| `POST /api/svp_relay/stop` | stop |

## Panel (ESPHome)

```yaml
simple_video_player:
  display_id: dsi_display
  speaker_id: usb_spk
  stream_port: 5000
  touchscreen_id: my_touch           # tap-to-stop
  channel_list:
    url: http://<ha-ip>:8123/api/svp_relay
    token: !secret ha_token
    widget_id: channel_dropdown      # an lvgl dropdown

lvgl:
  resume_on_input: false
```

Actions: `simple_video_player.channels.refresh`, `simple_video_player.channels.stop`.
