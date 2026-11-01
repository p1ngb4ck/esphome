# esphome-bluetooth

Bluetooth for ESPHome beyond BLE sensors: Bluetooth Classic, pairing that survives restarts, Classic and BLE
keyboards / mice / gamepads / remotes, BLE advertisement data from paired devices, streaming audio to Bluetooth
speakers and headphones (A2DP with AVRCP volume), and - on the ESP32-P4 - Bluetooth through the ESP32-C6
co-processor or a USB dongle.

USB Bluetooth dongles (`usb_bluetooth`) are in [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers);
the HID layer behind the keyboards (`hid`) in [esphome-hid](https://github.com/p1ngb4ck/esphome-hid).

## Getting started

A Bluetooth speaker as an ESPHome speaker (ESP32 or ESP32-S31, on-chip Bluetooth Classic):

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-bluetooth@main
    components: [esp32_ble, bt_classic, bt_pairing, a2dp_source, hid, storage]

esp32_ble:

bt_pairing:

a2dp_source:
  id: bt_audio
  target_name: Living Room Speaker

speaker:
  - platform: a2dp_source
    a2dp_source_id: bt_audio
    id: bt_speaker
```

A BLE keyboard:

```yaml
esp32_ble_tracker:

ble_client:
  - id: kbd_client
    mac_address: AA:BB:CC:DD:EE:FF

ble_client_hid:
  - id: ble_keyboard
    ble_client_id: kbd_client
    layout: de
```

Pairing records are kept with `bt_persistence` (on a storage or in the NVS), so devices reconnect after a restart.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option and action of `bt_pairing`, `bt_persistence`, `bt_hid`,
`ble_client_hid`, `ble_broadcast`, `a2dp_source` (speaker, device select, volume, microphone source, reconnect) and
the changes to `esp32_ble`, `esp32_ble_tracker` and `ble_client`.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/esp32-ble-start-order` | Bluedroid starts once WiFi's start-up is done and, with a Bluedroid USB dongle, once the dongle is ready; `restart()` | ESP32 with BLE | `pr/wifi-startup-done-callback` |
| `pr/esp32-ble-bt-logs` | `disable_bt_logs` also quiets the Bluedroid layers the build keeps | ESP32 with BLE | - |
| `pr/esp32-ble-nvs-ready` | esp32_ble waits for `nvs_ready` | ESP32 with BLE | `pr/esp32-nvs-ready` |
| `pr/bt-classic` | `bt_classic`: one Classic GAP callback with handlers, the shared IO capability and PIN type, dual mode on the ESP32-S31 | S31 on chip; others with a dongle | - |
| `pr/bt-persistence` | `bt_persistence`: Bluedroid's pairing data and GATT cache in PSRAM, saved to a storage file or the NVS | with esp32_ble or usb_bluetooth | `pr/storage` |
| `pr/bt-pairing-hid` | `bt_pairing` (pairing records, slots, device list, status events), `bt_hid` (Classic HID host), `ble_client_hid` (BLE HID), `ble_broadcast` (BLE advertisement data) | see CONFIGURATION.md | `pr/bt-classic`, `pr/hid`, `pr/storage` |
| `pr/esp32-ble-tracker-classic-scan-window` | the default BLE scan window stays 30 ms while Bluetooth Classic runs on the same radio | ESP32, S31 | - |
| `pr/esp32-ble-tracker-pause-scan` | `pause_scan` / `resume_scan`: BLE scanning paused while A2DP streams on the same radio | ESP32, S31 | - |
| `pr/a2dp-source` | `a2dp_source`: A2DP source with AVRCP - speaker, device select, volume, microphone source, reconnect, `idle_timeout` | ESP32, S31 on chip; others with a Bluedroid USB dongle | `pr/bt-pairing-hid`, `pr/esp32-ble-tracker-pause-scan` |
| `pr/bt-offload` | `bt_offload`: Bluetooth HID input and speaker offloaded to the ESP32-C6 (custom firmware), the peer data handlers of esp32_hosted on the P4 | P4 with C6 | `pr/usb-bluetooth` |
| `pr/ble-nus` | `ble_nus`: the UART debug callback with the prefix argument of the virtual UART work | ESP32 with BLE | `pr/uart-debugger-prefix-settings` |
| `pr/bt-host` | not in `main` yet: `bt_host`, one host component for BLE and Classic (pairing and persistence as its blocks, the controller per role, drivers on it); its README describes the YAML migration | as above | `main` |

`pr/storage` is in [esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface), `pr/hid` in
[esphome-hid](https://github.com/p1ngb4ck/esphome-hid), `pr/esp32-nvs-ready` in
[esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform), `pr/wifi-startup-done-callback` in
[esphome-network](https://github.com/p1ngb4ck/esphome-network), `pr/usb-bluetooth` in esphome-usb-drivers,
`pr/uart-debugger-prefix-settings` in [esphome-uart-tcp](https://github.com/p1ngb4ck/esphome-uart-tcp).

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

Thank you, [NonaSuomy](https://github.com/NonaSuomy), for many of the ideas behind the Bluetooth work, among them
offloading Bluetooth to the ESP32-C6. `a2dp_source` carries Phil Schatzmann's
[ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) library (Apache-2.0) - thank you for years of work on
Bluetooth audio for the ESP32. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
