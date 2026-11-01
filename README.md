# esphome-keyboard-layouts

ESPHome `dev` plus the keyboard layouts of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for keyboard_layouts. Branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/keyboard-layouts` | keyboard_layouts: a layout per keyboard, from sources in parallel: `directory` (storage), `relay: true` (Home Assistant through relay_link), `http_sources` (web servers serving `<base>/keyboard` and `<base>/keyboard?layout=<name>`); the list is all of them, a layout loads from storage, the relay, then the HTTP sources; device select and layout sensor | `pr/relay-link` ([esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link)), `pr/hid` ([esphome-hid](https://github.com/p1ngb4ck/esphome-hid)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | all |

## Keyboards

The layouts apply to the keyboards of `hid` inputs. The drivers that bring keyboards:
- [esphome-hid](https://github.com/p1ngb4ck/esphome-hid): `hid` (inputs, layouts, key sources), LVGL's HID input.
- [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers): `usb_hid` (USB keyboards, on USB host chips).
- [esphome-bluetooth](https://github.com/p1ngb4ck/esphome-bluetooth): `bt_hid` (Bluetooth Classic keyboards), `ble_client_hid` (BLE keyboards).
