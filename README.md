# esphome-browser

`browser`: a web browser on the ESP32-P4 display. A server on the network runs a full browser and offers it over VNC
or SPICE; the device shows it through `remote_desktop` (esphome-remote-desktop) - keyboard, mouse and touch work as
on a desktop. The heavy part (rendering the web) stays on the server, the device stays responsive.

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-browser@main
    components: [browser, remote_desktop, display, lvgl, hid, storage, usb_host]

remote_desktop:
  id: rdp
  lvgl_id: lvgl_main
  hid_ids: [keyboard, mouse]
  touchscreen_id: touch

browser:
  host: browser-server.local
  port: 5900

button:
  - platform: template
    name: Open browser
    on_press:
      - browser.open:
```

The server is any machine that runs a browser in a VNC or SPICE session (for example a container with a browser
and a VNC server).

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option and action.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/browser` | `browser`: a browser served over VNC / SPICE, opened through remote_desktop (`browser.open` / `close` / `is_open`) | `pr/remote-desktop` ([esphome-remote-desktop](https://github.com/p1ngb4ck/esphome-remote-desktop)) | P4 |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
