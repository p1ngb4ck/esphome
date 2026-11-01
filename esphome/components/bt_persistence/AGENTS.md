# bt_persistence: guide for AI agents

Bluedroid's own persistent data (bonds, link keys, the GATT client cache) kept in PSRAM and saved to a storage file
or the NVS.

- `idf/`: `esphome_nvs_store.*` and `esphome_nvs_redirect.h` - Bluedroid's NVS calls are redirected to an in-memory
  store. They are compiled into a copy of ESP-IDF's `bt` component made at build time (`_sync_bt_override()`, the
  same scheme as the exFAT FatFs override). Keep the redirect minimal: it must follow the IDF version's `bt` sources.
- `bt_persistence.*`: load before Bluetooth starts (waits for the storage, `storage_timeout`), write behind changes
  after `write_delay` through the storage worker, `set_path` at runtime, `on_ready` / `on_error`.
- The state is an `std::atomic` because Bluedroid's tasks write into the store while the main loop saves it.

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
