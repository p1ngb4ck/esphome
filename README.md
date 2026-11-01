# esphome-usb-host

A reworked `usb_host` for ESPHome: **plug in a device and it runs.** The host hands every device to the drivers by
what it is (its interface classes), so a keyboard, a USB stick, a camera or a sound card works without knowing its
vendor and product ID in advance. On the ESP32-P4 both USB controllers can run at once, and devices that start in
the wrong mode (Bluetooth dongles that enumerate as a driver CD, HID-proxy dongles) are switched automatically.

The device drivers (USB storage, optical drives, HID, UVC cameras, Bluetooth dongles, audio, serial adapters,
Ethernet) are in [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-usb-host@main
    components: [esp32, usb_host]

usb_host:
  enable_hubs: true
```

Then add the drivers you need from esphome-usb-drivers - each picks up its devices by class. Both controllers of an
ESP32-P4 (high-speed and full-speed port):

```yaml
usb_host:
  dual_host: true
  enable_hubs: true
```

## How routing works

`usb_host` hands each device to the drivers in this order: a driver whose configured filters name the device
(`vid` / `pid` / manufacturer / product), then a driver whose built-in table names it (devices that cannot be routed
by class), then the drivers of its interface classes. A driver gets the interfaces it matches, not the whole device,
so the other interfaces of the same device go to the drivers that match them (a camera's video beside its
microphone). `vid` / `pid` filters stay useful to give one particular device to one particular driver, and a
driver's `blacklist` keeps a device away from it. The serial number is read with every device's descriptors, so
devices of the same type can be told apart.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option - controllers, hubs, transfer limits, blacklists, mode
switching (hid2hci, usb_modeswitch messages, control requests), `tinyusb` beside `usb_host` on the ESP32-P4.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/usb-host-refactor` | routing by device class to the drivers (named > vendor table > class, interfaces per driver), several devices per client, one shared USB task, unified transfer engine (bulk, interrupt, control, isochronous streams), driver `blacklist`, host channel budget per controller, the patched espressif/usb component (per-port FIFO bias, `fsls_only`, external hub port requests, root port reset recovery) | S2, S3, S31, P4, H4 | `pr/esp32-loop-task-core`, `pr/esp32-idf-component-override-path` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |
| `pr/usb-host-dual-host` | `controllers` (`high_speed`, `full_speed`, `both`), `dual_host`, `fs_pins`, `full_speed_only` | P4; `full_speed_only`: P4, S31 | `pr/usb-host-refactor` |
| `pr/usb-host-mode-switch` | `mode_switch`: devices that enumerate in the wrong mode switched before they are routed - `hid2hci`, usb_modeswitch `bulk_messages`, any `control` request; switchable at runtime, kept in the preferences | as `usb_host` | `pr/usb-host-refactor` |
| `pr/tinyusb-port` | `tinyusb` `port` on the ESP32-P4 (`high_speed` / `full_speed`): a USB device on one controller beside `usb_host` on the other | P4 | `pr/usb-host-dual-host` |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

The mode switch methods follow BlueZ's `hid2hci` and Josua Dietze's usb_modeswitch - thank you for documenting
what all those dongles need. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
