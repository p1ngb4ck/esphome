# esphome-usb-host

ESPHome `dev` plus the `usb_host` rework of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`,
in three branches. `main` contains all three and is identical to `testing_dev`'s `usb_host`. Base: ESPHome `dev`
at `1f70f86b47`. The USB drivers (usb_hid, usb_storage, usb_uvc, usb_audio, usb_bluetooth, usb_ethernet) are in
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers), usb_uart in
[esphome-usb-uart](https://github.com/p1ngb4ck/esphome-usb-uart).

## Why

**Plug in and it runs.** Upstream's `usb_host` opens a device for a driver when the driver's `vid` / `pid` filter
names it. For drivers of whole device classes (keyboards, mice, gamepads, sticks, cameras, sound cards) that means a
user has to know the `vid` / `pid` of a device they may only buy later, and compile again for it. Users expect a
device to work when it is plugged in.

**The host routes, the drivers match.** Device enumeration is no general enumeration inside `usb_host` that every
driver walks on its own. `usb_host` hands each device to the drivers by what it is: a driver whose configured
filters name the device (`vid` / `pid` / manufacturer / product) wins, then a driver whose built-in table names it
(devices that cannot be routed by class), then the drivers of its interface classes. A driver gets the interfaces
it matches, not the whole device, so the other interfaces of the same device go to the drivers that match them
(a camera's video beside its microphone). The drivers then do their own `vid` / `pid` matching on what they are
given.

`vid` / `pid` filters stay where they make sense: to give one particular device to one particular driver, and a
per-driver `blacklist` (exact `vid` / `pid`, optionally single interfaces) keeps a device away from a driver. The
serial number is read with every device's descriptors (`usb_device_info_t`), so devices of the same type can be
told apart (usb_hid and usb_audio use it in their device key).

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/usb-host-refactor` | routing by device class to the drivers (named > vendor table > class, interfaces per driver), several devices per client, one shared USB task (drivers registered with the USB host library only while a device is bound to them), unified transfer engine (bulk, interrupt, control, isochronous streams), driver `blacklist`, host channel budget per controller, the patched espressif/usb component (`idf_usb_patch.py`: per-port FIFO bias, `fsls_only`, external hub port requests, root port reset recovery) | S2, S3, S31, P4, H4 | `pr/esp32-loop-task-core`, `pr/esp32-idf-component-override-path` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |
| `pr/usb-host-dual-host` | `controllers` (`high_speed`, `full_speed`, `both`), `dual_host`, `fs_pins` (full-speed PHY pins), `full_speed_only` | `controllers` / `dual_host` / `fs_pins`: P4; `full_speed_only`: P4, S31 | `pr/usb-host-refactor` |
| `pr/usb-host-mode-switch` | `mode_switch`: devices that enumerate in the wrong mode switched before they are routed - `hid2hci` (BlueZ methods `dell`, `csr`, `csr2`), usb_modeswitch `bulk_messages`, any `control` request; switchable at runtime, the choice kept in the preferences | all of the above | `pr/usb-host-refactor` |

The patched espressif/usb component stays whole in `pr/usb-host-refactor`: its later patches anchor on code the
per-port FIFO bias patch adds. `pr/usb-host-refactor` is `testing_dev`'s `usb_host` without the lines of the other
two branches; three lines that hold both read there without the other branch's part:
`from esphome.const import CONF_DEVICES, CONF_ID` (`__init__.py`), `this->route_(this->devices_.back());` in
`device_new_()` and `if (dev == nullptr)` in `take_device()` (`usb_host_component.cpp`).

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-usb-host@main
    components: [esp32, usb_host]

usb_host:
  dual_host: true
  fs_pins: "24_25"
  enable_hubs: true
  max_transfer_requests: 32
  max_packet_size: 512
  mode_switch:
    - vid: 0x0BDA
      pid: 0x1A2B
      bulk_messages:
        - "5553424312345678000000000000061b000000020000000000000000000000"
        - "5553424312345678000000000001061b000000020000000000000000000000"
      need_response: true
```

The `mode_switch` entry is the Realtek RTL8851BU dongle: it starts as a driver CD (`0BDA:1A2B`), the usb_modeswitch
StandardEject (SCSI eject on LUN 0 and 1) turns it into the Bluetooth / WLAN device (`3625:010B`).

- `dual_host: true` runs both ESP32-P4 controllers (`controllers: both`); `controllers: full_speed` runs the
  full-speed one only. Both need ESP-IDF 5.4 or newer.
- `fs_pins`: `26_27` (default) or `24_25`, the GPIO pair of the P4's full-speed controller.
- `full_speed_only: true`: the high-speed controller as a full / low-speed host.
- `mode_switch` entries take exactly one of `hid2hci`, `bulk_messages` (1 to 3 hex strings, with
  `need_response`) or `control` (`request_type`, `request`, `value`, `index`, `data`).

`external_components` replaces components only; the ESPHome core of the build host stays as installed. With the
other topics together use the combined branch `github://p1ngb4ck/esphome@topics-combined`.
