# ble_broadcast: guide for AI agents

BLE devices that only advertise (buttons, tags), paired through `bt_pairing` and heard without a connection.
`ble_broadcast.*` filters the tracker's advertisements by the paired address and fires `on_manufacturer_data` /
`on_service_data` on the main loop. Instances are usually slots made by `bt_pairing` (`max_broadcast_devices`).

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
