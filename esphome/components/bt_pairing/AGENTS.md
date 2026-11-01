# bt_pairing: guide for AI agents

One pairing database for every Bluetooth driver, and runtime slots for HID devices.

- `bt_pairing.*`: records (address, name, kind, transport), the database file on a storage (read once the storage is
  mounted, rewritten on change through the storage worker; a PSRAM I/O buffer) or the preferences, the pairing scan
  (Classic inquiry + BLE scan, `std::atomic` flags set from Bluedroid's tasks, results applied on the main loop),
  pair / connect / disconnect / unpair / forget, status messages.
- Slots: `max_hid_devices` / `max_broadcast_devices` make `bt_hid`, `ble_client` + `ble_client_hid` and
  `ble_broadcast` instances in codegen (`_generate_slots`), filled with a device at runtime; a paired device lands in
  a free slot of its kind and transport.
- `select/`: a device select.
- The record format is persistent: a change must read old records (or migrate them), or users lose their pairings.

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
