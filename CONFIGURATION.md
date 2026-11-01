# Configuration reference

The first steps are in [README.md](README.md). Bluetooth Classic needs Classic hardware: the ESP32 and ESP32-S31 on
chip, every other chip through a USB dongle (`usb_bluetooth` with `bluedroid: true`, esphome-usb-drivers). BLE runs
on chip, on the ESP32-P4 through the ESP32-C6 (`esp32_hosted`).

- [`bt_pairing`](#bt_pairing) - pairing records, HID slots, scan / pair actions
- [`bt_persistence`](#bt_persistence) - Bluedroid's pairing data across restarts
- [`bt_hid`](#bt_hid) - Bluetooth Classic HID devices
- [`ble_client_hid`](#ble_client_hid) - BLE HID devices
- [`ble_broadcast`](#ble_broadcast) - BLE advertisement data of paired buttons
- [`a2dp_source`](#a2dp_source) - audio to Bluetooth speakers
- [Changes to upstream components](#changes-to-upstream-components)

## `bt_pairing`

One database of paired devices for every Bluetooth driver, and the slots for HID devices that are paired at runtime
(no YAML entry per device).

| Option | Default | Description |
|---|---|---|
| `max_devices` | `8` | Paired devices kept, 1-32. |
| `max_hid_devices` | `0` | HID slots (keyboards, mice, remotes, gamepads) created for runtime pairing, 0-8. Classic: connected at once (Bluedroid's HID host takes at most 7 different devices per boot); BLE: paired at most. |
| `max_broadcast_devices` | `0` | Slots for BLE devices that only advertise (buttons), 0-8. |
| `file` | preferences | A file on a storage for the database; read once the storage is mounted, rewritten on every change. |
| `layout`, `num_lock` | `us`, `true` | Keyboard layout of the HID slots (see esphome-hid). |
| `on_key` | - | Any slot's key: `device`, `key`, `pressed`. `key:` filters. |
| `on_text` / `on_pointer` / `on_control` | - | As the HID input triggers, with `device` first. |
| `on_connected` / `on_disconnected` | - | `device`. |
| `on_status` | - | What the pairing scan and a pick do: `message`. |
| `esp32_ble_id` | - | With `esp32_ble_tracker`: the pairing scan also lists advertising BLE HID devices. |

Actions:

| Action | Options |
|---|---|
| `bt_pairing.scan` | `duration` (`20s`): list devices in pairing mode (Classic inquiry and BLE). |
| `bt_pairing.cancel_scan` | - |
| `bt_pairing.pair` / `connect` / `disconnect` / `unpair` / `forget` | `address` (required, templatable, `AA:BB:CC:DD:EE:FF`) |
| `bt_pairing.forget_all` | - |
| `bt_pairing.log` | Log the database. |

```yaml
bt_pairing:
  id: bt_db
  file: /sdcard/bluetooth/pairing.bin
  max_devices: 16
  max_hid_devices: 4
  layout: de
  on_status:
    - logger.log:
        format: "%s"
        args: [message.c_str()]

button:
  - platform: template
    name: Find Bluetooth devices
    on_press:
      - bt_pairing.scan:
          duration: 30s
```

## `bt_persistence`

Bluedroid's own pairing data (link keys, bonds) and optionally the GATT client cache, kept in PSRAM and written
behind changes.

| Option | Default | Description |
|---|---|---|
| `backend` | `storage` | `storage` (a file) or `nvs` (ESP-IDF's default). |
| `path` | required (`storage`) | The file on a storage, shorter than 192 characters. |
| `write_delay` | `2s` | Changes are written this long after the last one. |
| `storage_timeout` | `20s` | How long to wait for the storage at boot; then `on_error` reports it. |
| `gatt_cache` | unchanged | Persist the GATT client cache. |
| `on_ready` / `on_error` | - | The data is loaded / could not be loaded or written. |

```yaml
bt_persistence:
  backend: storage
  path: /sdcard/bluetooth/bt_store.bin
  gatt_cache: true
```

`bt_persistence.set_path` (`path`, templatable) moves the file at runtime.

## `bt_hid`

A Bluetooth Classic HID device (keyboard, mouse, gamepad, remote) as a HID input. Needs Classic (see top). Entries
are usually created by `bt_pairing`'s `max_hid_devices`; a fixed device can be configured directly.

| Option | Default | Description |
|---|---|---|
| `mac_address` | - | The device. Without it the entry is a slot: `bt_hid.pair` finds a device in pairing mode and `bt_pairing` keeps it across restarts. |
| `address_global` | - | A `globals` (std::string) that holds the address at runtime. |
| `pin` | `0000` | PIN for devices without Secure Simple Pairing. |
| `auto_connect` | `true` | Reconnect when the device is in reach. |
| `on_connect` / `on_disconnect` | - | Connection events. |

Plus every HID input option (`layout`, `on_key`, `on_text`, ...; esphome-hid).

Actions: `bt_hid.scan` (`duration` `10s`), `bt_hid.pair` (`id`, `duration` `30s`).

## `ble_client_hid`

A BLE HID device (HID over GATT) as a HID input, on a `ble_client`.

| Option | Default | Description |
|---|---|---|
| `ble_client_id` | required | The BLE client. |
| `encryption` | `true` | Bond and encrypt (most HID devices require it). |
| `protocol_mode` | `unchanged` | `report`, `boot` or `unchanged`. |
| `discovery_mode` | `standard` | `forensic` reads and subscribes everything the device offers (diagnosis). |
| `keepalive_interval` | `0s` | Keep the link up while the device would sleep, so no key press is lost to a reconnect. `0s` = off. |
| `conn_latency` | the device's | Connection events the device may skip (saves its battery), 0-499. |
| `publish_to_ha` | `true` | Fire `esphome.hid_events` in Home Assistant for every decoded value (needs `api:` and "Allow the device to perform Home Assistant actions"). |
| `event_sampling_interval` | `0ms` | Coalesce relative motion in those Home Assistant events; keys, text and `on_pointer` see every report. |

Plus every HID input option. Actions: `ble_client_hid.pair` (`id`, `duration` `30s`), `ble_client_hid.cancel_pairing`,
`ble_client_hid.unpair`.

## `ble_broadcast`

BLE devices that only advertise (buttons, tags), paired through `bt_pairing` and heard without a connection.

| Option | Description |
|---|---|
| `bt_pairing_id` | The pairing database. |
| `on_manufacturer_data` | Manufacturer data from the paired device. |
| `on_service_data` | Service data from the paired device. |

`ble_broadcast.unpair` forgets the device.

## `a2dp_source`

Streams an ESPHome speaker to a Bluetooth speaker or headphones (A2DP source, AVRCP volume). ESP32 / ESP32-S31 on
chip, other chips with a Bluedroid USB dongle.

| Option | Default | Description |
|---|---|---|
| `local_name` | `ESPHome Audio` | The device's Bluetooth name. |
| `target_name` | `""` | Pair with the device of this name; empty = the strongest device found in the pairing window. |
| `pin_code` | `0000` | PIN for devices without Secure Simple Pairing. |
| `pair_on_boot_if_empty` | `true` | Open the pairing window at boot when nothing is paired yet. |
| `settle_time` | `15s` | How long the pairing window collects devices before it picks. |
| `fallback_time` | `20s` | If the picked device does not answer, take another one after this long. |
| `reconnect` | `listen` | `listen` (wait for the speaker to connect) or `paging` (page it every `paging_interval`). |
| `paging_interval` | `30s` | With `reconnect: paging`. |
| `idle_timeout` | `1s` | Suspend the stream (link kept) after this long without audio; `0s` keeps it running. |
| `volume` | `60` | Start volume, 0-127 (AVRCP). |
| `microphone` | - | A stereo microphone source streamed instead of a speaker's audio (never idles). |
| `buffer_duration` | `100ms` | Slack between a microphone and the Bluetooth clock. |
| `on_paired` / `on_connected` / `on_disconnected` / `on_streaming_start` / `on_streaming_stop` | - | Events. |

Platforms (`a2dp_source_id`): `speaker` (the audio input), `select` (paired devices, pick one to connect), `number`
(volume).

Actions: `a2dp_source.start_pairing`, `stop_pairing`, `forget_device`, `pause_reconnect`, `resume_reconnect`,
`pair_with_name` (`name`, templatable). Conditions: `a2dp_source.is_connected`, `is_streaming`, `is_paired`.

```yaml
a2dp_source:
  id: bt_audio
  target_name: Kitchen Speaker
  reconnect: paging
  on_connected:
    - logger.log: speaker connected

speaker:
  - platform: a2dp_source
    a2dp_source_id: bt_audio
    id: bt_speaker

select:
  - platform: a2dp_source
    a2dp_source_id: bt_audio
    name: Bluetooth speaker

number:
  - platform: a2dp_source
    a2dp_source_id: bt_audio
    name: Bluetooth volume
```

## Changes to upstream components

| Component | Change |
|---|---|
| `esp32_ble` | Starts after WiFi's start-up and after a Bluedroid USB dongle is ready; `restart()`; waits for `nvs_ready`; `disable_bt_logs` also quiets the Bluedroid layers the build keeps; `io_capability` shared with Classic. |
| `esp32_ble_tracker` | The default scan window stays 30 ms while Classic shares the radio; `pause_scan()` / `resume_scan()` (C++) while A2DP streams. |
| `ble_client` | `mac_address` optional: a client slot filled by `bt_pairing` at runtime. |
| `esp32_hosted` | ESP32-P4 with ESP32-C6: the peer data handlers `bt_offload` uses. |
| `ble_nus` | The UART debug callback with the prefix argument (esphome-uart-tcp). |
| `bt_offload` | ESP32-P4: Bluetooth HID input and speaker offloaded to an ESP32-C6 running a custom firmware. No options. |
| `bt_classic` | The shared Classic GAP; loaded by the drivers, no options. |
