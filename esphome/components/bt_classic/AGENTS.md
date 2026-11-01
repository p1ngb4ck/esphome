# bt_classic: guide for AI agents

The shared Bluetooth Classic GAP. ESP-IDF allows one GAP callback; `bt_classic.*` registers it
(`esp_bt_gap_register_callback`) and dispatches each event to the handlers the drivers register (`bt_hid`,
`a2dp_source`, `bt_pairing`), with fallbacks for unhandled events. It owns the shared IO capability and PIN type and
the BTDM dual mode on the ESP32-S31. Drivers never register their own GAP callback.

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
