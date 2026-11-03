# usb_bluetooth: guide for AI agents

Bluetooth dongles as HCI controllers. The Bluetooth host side lives in esphome-bluetooth (`esp32_ble`, `bt_classic`,
...); this component is the transport.

- `usb_bluetooth.*`: the USB HCI transport (commands by control transfer, events on interrupt IN, ACL on bulk, SCO
  on isochronous), the virtual HCI that ESP-IDF's Bluedroid talks to (`bluedroid: true`), start-up ordering with
  `esp32_ble` (Bluedroid starts only once the dongle is ready).
- `realtek.py` + firmware download: Realtek dongles get their firmware (and config) at start, validated and packed
  at build time.
- `hci_split.*`: BLE and Classic split across two controllers (the chip's own BLE radio or a co-processor and the
  dongle for Classic).
- `c6_offload.*`: on an ESP32-P4 with an ESP32-C6, the C6 runs the host stack for the dongle.

Rules:
- HCI traffic runs on the USB task and Bluedroid's tasks; nothing from there touches ESPHome entities directly.
- The SCO interface does not fit every controller's channel budget; it is claimed only when a profile needs it.
- A dongle that starts in the wrong mode is switched by `usb_host`'s `mode_switch`, not here.
