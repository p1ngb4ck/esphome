# esphome-hid

ESPHome `dev` plus the HID input of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`: the `hid`
component (report maps, keyboard layouts, keyboards, mice, gamepads, remotes as inputs for any transport) and LVGL
driven by them (HID keyboards and pointers, key bindings, input from outside). One branch per change; `main`
contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`:
`hid` is identical, the lvgl lines are those `git blame` assigns to these commits.

The transports are in other topics: `usb_hid` in [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers),
`bt_hid` / `ble_client_hid` in esphome-bluetooth.

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/hid` | `hid`: report maps, keyboard layouts (also as files), keyboards, mice, gamepads, remotes; key events carry their device; `on_key`, `on_text`, `on_pointer`, `on_control`, API actions | all | - |
| `pr/lvgl-hid-input` | `lvgl` `hid_keyboards` and `hid_pointers` (cursor look, `hide_after`, speed, acceleration, wheel, key mode, a gamepad touchpad as pointer or touch), `key_bindings` (built-in navigation, combos up to 3 keys, `reset`, a key file on a storage), keys / text / pointer handed to another component while LVGL is paused | all | `pr/hid` |
| `pr/lvgl-remote-input` | `lvgl` `remote_input`: LVGL from outside (pointer, keys by key_bindings action, text), keys fire their action while LVGL is paused | all | `pr/lvgl-hid-input` |

The paused hand-over (`set_paused_input`, which simple_video_player uses to send keys, text and the mouse to a
udisp sender) sits in the same lines as
the HID key and pointer path, so it is part of `pr/lvgl-hid-input`. `key_bindings` `file:` needs the storage
interface ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)).

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-hid@main
    components: [hid, lvgl]

lvgl:
  hid_pointers:
    - id: mouse_pointer
      hid_id: [kbd]
      hide_after: 10s
  remote_input:
    id: lv_remote
  hid_keyboards:
    - kbd
  key_bindings:
    id: key_bindings
    actions:
      - name: back
        then:
          - lvgl.page.previous:
```

`kbd` is a HID input, for example a `usb_hid` block. `external_components` replaces components only; the ESPHome
core of the build host stays as installed. With the other topics together use the combined branch
`github://p1ngb4ck/esphome@topics-combined`.
