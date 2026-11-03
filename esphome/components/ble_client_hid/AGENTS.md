# ble_client_hid: guide for AI agents

A BLE HID device (HID over GATT) on a `ble_client`, as a `hid::HIDInput` (read `esphome/components/hid/AGENTS.md`).

- `ble_client_hid.*`: service discovery (`standard` or `forensic`), report map read, CCCD subscriptions
  (`subscription_state.h`), protocol mode, bonding / encryption, keepalive while the device sleeps, connection
  latency.
- `ordered_event_buffer.h`: notifications from the BLE task are kept in order and drained on the main loop.
- `sensor.py`, `text_sensor.py`, `select/`: last-event entities and the device select; `publish_to_ha` fires
  `esphome.hid_events` (coalesced by `event_sampling_interval`).

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
