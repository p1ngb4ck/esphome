# bt_hid: guide for AI agents

A Bluetooth Classic HID device as a `hid::HIDInput` (read `esphome/components/hid/AGENTS.md`).

- `bt_hid.*`: ESP-IDF's HID host (`esp_hidh_api.h`): open / close, the report descriptor handed to `hid`, input reports
  (`ESP_HIDH_DATA_IND_EVT`), output reports (keyboard LEDs), reconnect with `auto_connect`, PIN for legacy pairing.
- An entry without `mac_address` is a slot that `bt_hid.pair` or `bt_pairing` fills at runtime.
- Bluedroid's HID host accepts at most 7 different devices per boot; slots are bounded accordingly.
- `select/`: device select.

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
