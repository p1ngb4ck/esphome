# Configuration reference

The first steps are in [README.md](README.md). ESP32-P4 only; needs `remote_desktop`.

## `browser`

| Option | Default | Description |
|---|---|---|
| `remote_desktop_id` | the only one | The `remote_desktop` that shows the browser. |
| `host` | required | The browser server. |
| `port` | `5900` | Its VNC / SPICE port. |
| `protocol` | `auto` | `vnc`, `spice` or `auto` (probe). |
| `password` | `""` | The session password. |

## Actions and condition

| Action | Description |
|---|---|
| `browser.open` | Connect `remote_desktop` to the browser server (a remote desktop session of its own). |
| `browser.close` | End the browser's session; a remote desktop session opened otherwise is left alone. |

Condition `browser.is_open`: the browser's own session is connected.

```yaml
browser:
  host: browser-server.local
  protocol: spice
  password: !secret browser_password

lvgl:
  pages:
    - id: browser_page
      on_load:
        - browser.open:
      on_unload:
        - browser.close:
```
