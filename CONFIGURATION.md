# Configuration reference

The first steps are in [README.md](README.md).

## `usb_host`

| Option | Default | Description |
|---|---|---|
| `enable_hubs` | `false` | Devices behind USB hubs. |
| `max_transfer_requests` | `16` | Transfers in flight per client, 1-32. |
| `max_packet_size` | 512 on ESP32-P4 / S31, else 64 | Largest bulk / interrupt packet a transfer buffer holds: 64, 128, 256, 512 or 1024. |
| `controllers` | `high_speed` | ESP32-P4: which controller(s) run as host - `high_speed`, `full_speed` or `both`. |
| `dual_host` | `false` | ESP32-P4: both controllers as hosts (same as `controllers: both`). |
| `fs_pins` | `26_27` | ESP32-P4: the GPIO pair of the full-speed controller, `26_27` or `24_25`. |
| `full_speed_only` | `false` | ESP32-P4 / S31: the high-speed controller as a full / low-speed host. |
| `mode_switch` | - | Devices switched to their working mode before they are routed, see below. |

Both controllers need ESP-IDF 5.4 or newer.

```yaml
usb_host:
  dual_host: true
  fs_pins: "24_25"
  enable_hubs: true
  max_transfer_requests: 32
```

### Options every USB driver has

Drivers built on `usb_host` (esphome-usb-drivers) take these in their device entries:

| Option | Description |
|---|---|
| `vid` / `pid` | Give this one device to this entry (hex, `0x046D`). Without them the entry takes any device of its class. |
| `manufacturer` / `product` | Match the device's descriptor strings. |
| `blacklist` | Devices this driver never takes: a list of `vid`, `pid` and optionally `interfaces` (interface numbers 0-31; without it the whole device). |

```yaml
usb_hid:
  - id: any_keyboard
    blacklist:
      - vid: 0x3625
        pid: 0x010B
        interfaces: [2]
```

### `mode_switch`

Each entry names the device in its wrong mode (`vid`, `pid`, both required) and exactly one way to switch it:

| Option | Description |
|---|---|
| `hid2hci` | BlueZ's hid2hci method for Bluetooth dongles in HID proxy mode: `dell`, `csr` or `csr2`. |
| `bulk_messages` | 1-3 usb_modeswitch `MessageContent` hex strings sent to the first bulk OUT endpoint. `need_response` (`false`): read the device's answer after each message. |
| `control` | Any control request: `request_type`, `request` (both required, hex), `value` (`0`), `index` (`0`), `data` (`[]`, bytes). |

A Realtek Bluetooth / WLAN dongle that starts as a driver CD (`0BDA:1A2B`) and becomes `3625:010B` after a SCSI
eject on LUN 0 and 1:

```yaml
usb_host:
  mode_switch:
    - vid: 0x0BDA
      pid: 0x1A2B
      bulk_messages:
        - "5553424312345678000000000000061b000000020000000000000000000000"
        - "5553424312345678000000000001061b000000020000000000000000000000"
      need_response: true
```

A CSR dongle in HID proxy mode (use the IDs your dongle shows in that mode):

```yaml
usb_host:
  mode_switch:
    - vid: 0x0A12
      pid: 0x1000
      hid2hci: csr
```

## `tinyusb` beside `usb_host` (ESP32-P4)

The ESP32-P4 has two USB controllers: `tinyusb` (the device acting as a USB device) and `usb_host` can run at the
same time on different ones.

| Option | Default | Description |
|---|---|---|
| `port` | `full_speed` | `high_speed` or `full_speed`: the controller `tinyusb` uses. `vbus_monitor_pin` is not supported on `high_speed`. |

```yaml
tinyusb:
  port: full_speed

usb_host:
  controllers: high_speed
```

On other chips `tinyusb` and `usb_host` share the one controller and cannot be combined.
