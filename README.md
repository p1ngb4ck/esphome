# esphome-remote-desktop

`remote_desktop`: a VNC and SPICE client app for the ESP32-P4. The remote screen is drawn into an LVGL canvas
through the display's video output (Tight JPEG and SPICE MJPEG through the hardware JPEG decoder); keyboards, mice
and the touchscreen control the remote machine; SPICE sound plays on an ESPHome speaker; USB devices can be
redirected to the remote machine. No server in between - the device connects directly.

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-remote-desktop@main
    components: [remote_desktop, display, lvgl, hid, storage, usb_host]

remote_desktop:
  id: rdp
  lvgl_id: lvgl_main
  hid_ids: [keyboard, mouse]
  touchscreen_id: touch
  speaker: main_speaker

button:
  - platform: template
    name: Connect to server
    on_press:
      - remote_desktop.connect:
          host: 192.168.1.50
          port: 5900
          password: !secret vnc_password
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option and action - protocols (`vnc`, `spice`, `auto`), saved
connections, the close gesture, the connection select.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/remote-desktop` | `remote_desktop`: VNC (RFB, Tight JPEG through the hardware decoder) and SPICE (MJPEG streams, JPEG images through the hardware decoder) on a page canvas of the video output; keys / mouse / touches, cursor, SPICE sound, USB redirection (usbredir; USB/IP with VNC), saved connections, probing (`protocol: auto`), close gestures | `pr/display-video-output-page-draw` ([esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)), `pr/usb-host-refactor` ([esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host)) | P4 |
| `pr/remote-desktop-close-action` | the close gesture a configured key binding (`close_action`, default `home`; Back stays a normal key) | `pr/remote-desktop` | P4 |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
