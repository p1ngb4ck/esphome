# esphome-hid

HID input for ESPHome: one `hid` component that understands keyboards, mice, gamepads and remotes from any
transport (USB, Bluetooth Classic, BLE), with keyboard layouts for about a hundred languages, and LVGL driven by
them - keyboard navigation, a mouse cursor, key bindings that users can learn on the device, and input from outside
(Home Assistant, scripts).

The transports are in other repositories: `usb_hid` in
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers), `bt_hid` / `ble_client_hid` in
[esphome-bluetooth](https://github.com/p1ngb4ck/esphome-bluetooth).

## Getting started

A USB keyboard and mouse that operate an LVGL UI:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-hid@main
    components: [hid, lvgl]
  - source: github://p1ngb4ck/esphome-usb-drivers@main
    components: [usb_host, usb_hid]

usb_host:
  enable_hubs: true

usb_hid:
  - id: keyboard
    layout: de
  - id: mouse

lvgl:
  hid_keyboards:
    - hid_id: keyboard
  hid_pointers:
    - hid_id: mouse
      hide_after: 10s
```

Arrow keys, Tab, Enter and Escape navigate the UI out of the box. React to keys in an automation:

```yaml
usb_hid:
  - id: keyboard
    on_key:
      - key: F1
        then:
          - logger.log: help pressed
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option - layouts and custom keys, all triggers with their variables,
gamepad rumble and LEDs, the `hid` sensors, LVGL keyboards / pointers / remote input, key bindings (built-in
actions, combos, learning, a key file).

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/hid` | `hid`: report maps, keyboard layouts (also as files), keyboards, mice, gamepads, remotes; key events carry their device; `on_key`, `on_text`, `on_pointer`, `on_control`, API actions | all | - |
| `pr/lvgl-hid-input` | `lvgl` `hid_keyboards` and `hid_pointers`, `key_bindings` (built-in navigation, combos up to 3 keys, learning, a key file on a storage), keys / text / pointer handed to another component while LVGL is paused | all | `pr/hid` |
| `pr/lvgl-remote-input` | `lvgl` `remote_input`: LVGL from outside (pointer, keys by key binding action, text), keys fire their action while LVGL is paused | all | `pr/lvgl-hid-input` |

`key_bindings: file:` needs the storage interface
([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)).

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

The keyboard layouts are generated from freedesktop.org's
[xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config) - thank you for the
world's keyboards in one place. Thank you, [NonaSuomy](https://github.com/NonaSuomy), for many of the ideas behind
the HID work. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
