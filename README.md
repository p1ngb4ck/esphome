# esphome-bluetooth

ESPHome `dev` plus the Bluetooth host and HID parts of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome)
`testing_dev` as they are today: Classic GAP dispatch, pairing records and slots, Bluedroid data persistence,
Classic and BLE HID, BLE advertisement data, Bluetooth audio (A2DP source), the Bluedroid start order, the C6 offload. One branch per change;
`main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from
`testing_dev` (components identical; the lines in esp32_ble, ble_client, api and esp32_hosted assigned by
`git blame`). The USB dongle (`usb_bluetooth`) is in
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

The rework into one host component (scan, pairing, security, bonding for BLE and Classic) with the profiles as
drivers is on `pr/bt-host` (on top of `main`); the other branches are the state it starts from.

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/esp32-ble-start-order` | Bluedroid starts once WiFi reports its start-up done and, with a usb_bluetooth bluedroid dongle, once the dongle is ready; `restart()` | ESP32 with BLE | `pr/wifi-startup-done-callback` |
| `pr/esp32-ble-bt-logs` | `disable_bt_logs` also quiets the Bluedroid layers the build keeps (ERROR instead of their defaults) | ESP32 with BLE | - |
| `pr/esp32-ble-nvs-ready` | esp32_ble waits for `nvs_ready` | ESP32 with BLE | `pr/esp32-nvs-ready` |
| `pr/bt-classic` | `bt_classic`: one Classic GAP callback with handlers and fallbacks, the shared IO capability and PIN type, BTDM dual mode on the ESP32-S31 | S31 on chip; others with a dongle | - |
| `pr/bt-persistence` | `bt_persistence`: Bluedroid's pairing data and GATT cache in PSRAM, saved to a storage file (`backend: storage`) or NVS | with esp32_ble or usb_bluetooth | `pr/storage` |
| `pr/bt-pairing-hid` | `bt_pairing` (pairing records, slots, device list, status events), `bt_hid` (Classic HID host), `ble_client_hid` (BLE HID), `ble_broadcast` (BLE advertisement data) with the ble_client / esp32_ble / api hooks they use; one branch because `bt_pairing` creates the slots of the other three | see below | `pr/bt-classic`, `pr/hid`, `pr/storage` |
| `pr/esp32-ble-tracker-classic-scan-window` | the defaulted BLE scan window stays 30 ms when Bluetooth Classic runs on the same radio | ESP32, S31 | - |
| `pr/esp32-ble-tracker-pause-scan` | `pause_scan` / `resume_scan`: BLE scanning paused while A2DP streams on the chip's own radio | ESP32, S31 | - |
| `pr/a2dp-source` | `a2dp_source`: A2DP source with AVRCP (vendored ESP32-A2DP library) - speaker, device select, volume, microphone source, reconnect, `idle_timeout` | ESP32 on chip; S31 on chip (with esp32_ble); others with a usb_bluetooth bluedroid dongle | `pr/bt-pairing-hid`, `pr/esp32-ble-tracker-pause-scan` |
| `pr/bt-host` | `bt_host`: `bt_classic`, `bt_pairing` and `bt_persistence` as one host component (`pairing:` / `persistence:` blocks); the drivers' discovery, SSP, bond removal and scan mode through it; `ble_controller` / `classic_controller` per role; Classic HID on the chip's own Classic (ESP32, ESP32-S31) | every chip with Bluedroid | `main` |
| `pr/bt-offload` | `bt_offload`: Bluetooth HID input and speaker offloaded to the C6 (custom firmware), the 16 peer data handlers of esp32_hosted on the P4 | P4 with C6 | `pr/usb-bluetooth` |

Chips as the code declares today: `ble_client_hid` and `ble_broadcast` on every chip with BLE (the P4 through the
C6); `bt_hid` (Classic HID) needs a `usb_bluetooth` dongle with `bluedroid: true` on every chip, the ESP32 and
ESP32-S31 included, although both have Classic on chip [seen bt_hid/__init__.py:106, bt_pairing/__init__.py:313].
That is one of the points of the planned rework.

`pr/storage` is in [esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface), `pr/hid` in
[esphome-hid](https://github.com/p1ngb4ck/esphome-hid), `pr/esp32-nvs-ready` in
[esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform), `pr/wifi-startup-done-callback` in
[esphome-network](https://github.com/p1ngb4ck/esphome-network), `pr/usb-bluetooth` in esphome-usb-drivers.

## Contributions

- [NonaSuomy](https://github.com/NonaSuomy): ideas behind the Bluetooth work, among them the C6 offload firmware
  (`bt_offload`).

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-bluetooth@main
    components: [esp32, esp32_ble, ble_client, bt_classic, bt_pairing, bt_hid, ble_client_hid, ble_broadcast,
                 bt_persistence, hid, storage, wifi, esp32_ble_tracker, a2dp_source]

esp32_ble:
  enable_on_boot: true
  io_capability: display_only

esp32_ble_tracker:

bt_pairing:
  id: bt_pairing_db
  file: /sdcard/bluetooth/bt_pairing.bin
  max_devices: 32
  max_hid_devices: 7
  max_broadcast_devices: 1
  layout: de

bt_persistence:
  id: bt_store
  backend: storage
  path: /sdcard/bluetooth/bt_store.bin
  write_delay: 2s
  storage_timeout: 20s
  gatt_cache: true
```

With `pr/bt-host` the same as one block (the preference keys are unchanged, so stored pairings stay):

```yaml
bt_host:
  classic_controller: usb
  ble_controller: c6
  pairing:
    id: bt_pairing_db
    file: /sdcard/bluetooth/bt_pairing.bin
    max_devices: 32
    max_hid_devices: 7
    max_broadcast_devices: 1
    layout: de
  persistence:
    id: bt_store
    backend: storage
    path: /sdcard/bluetooth/bt_store.bin
    write_delay: 2s
    storage_timeout: 20s
    gatt_cache: true
```

Actions `bt_pairing.*` are `bt_host.*`, `bt_persistence.set_path` is `bt_host.persistence_set_path`, the select
platform `bt_pairing` is `bt_host`, `bt_pairing_id` is `bt_host_id`; `external_components` lists `bt_host` instead of
`bt_classic`, `bt_pairing`, `bt_persistence`.

From the Waveshare ESP32-P4 Pi board's config (BLE on the C6, Classic on a USB dongle). `external_components`
replaces components only; the ESPHome core of the build host stays as installed. With the other topics together
use the combined branch `github://p1ngb4ck/esphome@topics-combined`.
