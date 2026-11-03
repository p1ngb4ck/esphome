# esphome-keyboard-layouts

`keyboard_layouts`: a keyboard layout per connected keyboard, chosen at runtime. The layouts are files - on a storage,
from Home Assistant or from a web server - so a firmware does not carry all hundred layouts, and a user picks theirs
from a select in Home Assistant or on the device. The choice is remembered per keyboard.

Keyboards come from the `hid` inputs of [esphome-hid](https://github.com/p1ngb4ck/esphome-hid): `usb_hid`
([esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers)), `bt_hid` and `ble_client_hid`
([esphome-bluetooth](https://github.com/p1ngb4ck/esphome-bluetooth)).

## Getting started

Write the layout files once: `script/export_hid_layouts.py <folder>` (in esphome-hid and `dev`) creates one `<name>.kbl`
per layout and an `index.tsv`. Copy the folder to the SD card, then:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-keyboard-layouts@main
    components: [keyboard_layouts, hid, storage]

keyboard_layouts:
  directory: /sdcard/keyboard
  keyboards:
    - keyboard
  device_select:
    name: Keyboard
  layout_sensor:
    name: Keyboard layout
```

`keyboard` is a HID input, for example a `usb_hid` entry.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option, layouts from Home Assistant and HTTP servers, the file
format.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/keyboard-layouts` | `keyboard_layouts`: a layout per keyboard from sources in parallel - `directory` (storage), `relay: true` (Home Assistant through relay_link), `http_sources`; device select and layout sensor | `pr/relay-link` ([esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link)), `pr/hid` ([esphome-hid](https://github.com/p1ngb4ck/esphome-hid)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | all |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).
- Third-party code keeps its own licence:
  - Layout files exported from `hid/layouts/`: generated from xkeyboard-config (freedesktop.org), MIT-style licence.

## Thanks

The layouts are generated from freedesktop.org's
[xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config). Everyone this project builds
on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
