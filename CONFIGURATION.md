# Configuration reference

The first steps are in [README.md](README.md). ESP32-P4 only.

## `remote_desktop`

| Option | Default | Description |
|---|---|---|
| `lvgl_id` | required | The LVGL instance whose display's video output shows the remote screen. |
| `hid_ids` | `[]` | Up to 4 HID inputs (keyboards, mice) sent to the remote machine. |
| `touchscreen_id` | - | Touches sent as pointer events; the two-finger hold ends the session. |
| `speaker` | - | SPICE sound output. |
| `connections` | `8` | Saved connections, 1-16. |
| `connections_file` | preferences | A file on a storage for the saved connections instead of the preferences. |
| `key_bindings_id` | - | LVGL key bindings: the close action comes from there. |
| `close_action` | `home` | The key binding action that ends the session when held. |

```yaml
remote_desktop:
  id: rdp
  lvgl_id: lvgl_main
  hid_ids: [keyboard, mouse]
  touchscreen_id: touch
  speaker: main_speaker
  connections: 8
  connections_file: /sdcard/remote/connections.txt
  key_bindings_id: key_bindings
```

## Actions and condition

| Action | Options |
|---|---|
| `remote_desktop.connect` | `host`, `port` (required), `protocol` (`vnc` (default), `spice`, `auto` = probe), `password` - all templatable. |
| `remote_desktop.connect_saved` | `index` 0-15, `password` (when not saved). |
| `remote_desktop.save` | `index` 0-15, `host` (required), `port` (`5900`), `name`. |
| `remote_desktop.forget` | `index`. |
| `remote_desktop.disconnect` | - |

Condition `remote_desktop.is_connected`.

```yaml
- remote_desktop.save:
    id: rdp
    index: 0
    name: Office PC
    host: office-pc.local
    port: 5900
- remote_desktop.connect_saved:
    id: rdp
    index: 0
```

## USB redirection (`select`)

Up to 4 selects (`platform: remote_desktop`, needs `usb_host`), each a slot that redirects one USB device to the
remote machine (usbredir with SPICE, USB/IP with VNC). The options are the connected USB devices and `None`.

```yaml
select:
  - platform: remote_desktop
    remote_desktop_id: rdp
    name: USB redirect 1
```
