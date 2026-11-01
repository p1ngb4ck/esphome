# bt_offload: guide for AI agents

ESP32-P4 with an ESP32-C6 co-processor: Bluetooth HID input and a Bluetooth speaker run on the C6 (a custom C6
firmware); the P4 receives HID reports and sends audio over `esp32_hosted`'s peer data channel.

- `bt_offload.*`: `BTOffload` (a `hid::HIDInput`), the peer data handlers, connection state (`std::atomic`).
- `speaker/`: `BTOffloadSpeaker`, audio to the C6.
- Peer data arrives on esp32_hosted's task; it is queued and handed to `hid` on the main loop.

Bluetooth threading (all Bluetooth components):
- Bluedroid callbacks (GAP, HID host, GATT, A2DP) run on Bluedroid's tasks. They copy what they need, set atomics or
  queue events, and return; entities, triggers, storages and LVGL are touched on the main loop only.
- Long-lived buffers (records, reports, audio) live in PSRAM; nothing grows per event.
