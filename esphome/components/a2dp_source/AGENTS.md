# a2dp_source: guide for AI agents

A2DP source with AVRCP: an ESPHome speaker (or a stereo microphone source) streamed to a Bluetooth sink.

- `BluetoothA2DP*.{h,cpp}`, `A2DPVolumeControl.h`, `config.h`: Phil Schatzmann's ESP32-A2DP library, vendored
  (`LICENSE.ESP32-A2DP`, Apache-2.0). Keep changes to it small and marked by their purpose in the commit; the
  ESPHome logic lives in `a2dp_source.*`.
- `a2dp_source.*`: pairing window (collect for `settle_time`, pick the strongest or `target_name`, fall back after
  `fallback_time`), reconnect (`listen` / `paging`), the audio ring between the speaker and the A2DP data callback,
  `idle_timeout` suspend, volume, BLE scan pause while streaming (`esp32_ble_tracker` `pause_scan()`), triggers.
- `speaker/`, `select/`, `number/`: the speaker input, the paired-device select, the volume.
- The A2DP data callback runs on Bluedroid's task and only reads the ring buffer; it never blocks. The library's
  app task (`BtAppT`) is created by the library.

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
