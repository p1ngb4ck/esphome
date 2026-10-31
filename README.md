# esphome-usb-drivers

ESPHome `dev` plus the USB device drivers of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`
on the `usb_host` rework ([esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host)): mass storage, optical
drives, HID, UVC cameras, Bluetooth dongles, USB audio. One branch per driver or change; `main` contains every
branch and esphome-usb-host's `main`. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from
`testing_dev` (all files of these components identical). `usb_ethernet` follows with the network topic (it needs
the network changes); `usb_uart` is in [esphome-usb-uart](https://github.com/p1ngb4ck/esphome-usb-uart).

Every driver is matched by device class: a device works when it is plugged in, `vid` / `pid` only to give one
particular device to one particular block, `blacklist` to keep a device away from a block.

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/usb-storage` | `usb_storage`: USB mass storage (bulk-only, SCSI) on the storage interface, several devices and LUNs, FAT / exFAT | S2, S3, S31, P4, H4 | `pr/storage`, `pr/usb-host-refactor` |
| `pr/usb-cdrom` | `usb_cdrom`: USB optical drives, ISO 9660 through the storage interface | S2, S3, S31, P4, H4 | `pr/usb-storage` |
| `pr/usb-hid` | `usb_hid`: keyboards, mice, gamepads, remotes, any number at once, through the `hid` component | S2, S3, S31, P4 | `pr/usb-host-refactor`, `pr/hid` |
| `pr/usb-uvc` | `usb_uvc`: USB video class cameras (MJPEG over isochronous streams), any number at once | S2, S3, S31, P4 | `pr/usb-host-refactor`, `pr/camera-encoder-h264` |
| `pr/usb-bluetooth` | `usb_bluetooth`: Bluetooth dongles as HCI controller - Bluedroid on the dongle (`bluedroid`), BLE / Classic split across two controllers, firmware download (`firmware`) | S2, S3, S31, P4 | `pr/usb-host-refactor`, `pr/esp32-ble-start-order` |
| `pr/usb-audio` | `usb_audio` rework: several devices per driver, each in a format it offers, streams open only while used (`stream_idle_timeout`), class requests on a control task, `feedback`, `volume_curve` / volume encoding quirks, up to 8 channels | S2, S3, S31, P4 | `pr/usb-host-refactor` |
| `pr/usb-audio-dac` | `usb_audio` `audio_dac` platform: the device's volume and mute | as `usb_audio` | `pr/usb-audio` |
| `pr/usb-audio-media-player` | `usb_audio` `media_player` platform | as `usb_audio` | `pr/usb-audio` |

Branches from other topics, contained as merges:

| Branch | Topic | What |
|---|---|---|
| `pr/storage` | [esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface) | the storage interface |
| `pr/usb-host-refactor` | [esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host) | routing by class, transfer engine |
| `pr/hid` | esphome-hid | the `hid` component |
| `pr/camera-encoder-h264` | esphome-camera | `camera` / `camera_encoder` H.264 streams, key frames |
| `pr/wifi-startup-done-callback` | esphome-network | `wifi` `add_on_startup_done_callback` |
| `pr/esp32-ble-start-order` | esphome-bluetooth | Bluedroid starts after WiFi's start-up and, with a bluedroid dongle, once the dongle is ready |

`usb_bluetooth` is the dongle path. Chips with Bluetooth Classic and BLE on chip (ESP32, ESP32-S31) need no dongle
for Bluetooth audio; `usb_bluetooth` is allowed on the S31 as well (an external radio instead of the onboard one).
The 8-channel path of `audio` / `speaker` / `mixer` is in esphome-audio.

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-usb-drivers@main
    components: [esp32, storage, usb_host, usb_storage, usb_cdrom, usb_hid, hid, usb_uvc, camera, camera_encoder,
                 usb_bluetooth, esp32_ble, wifi, usb_audio]

usb_host:
  dual_host: true
  enable_hubs: true

usb_storage:
  id: usb_storage_id
  devices:
    - id: usb_storage_drive
      mount_path: "/usb"
      vid: 0
      pid: 0

usb_cdrom:

usb_hid:
  - id: kbd
    layout: de(nodeadkeys)
    on_text:
      - logger.log:
          format: "text '%s'"
          args: [text.c_str()]

usb_uvc:
  id: usb_cam

usb_bluetooth:
  id: usb_bt
  vid: 0x3625
  pid: 0x010B
  firmware: rtl8851bu_fw.bin
  bluedroid: true

usb_audio:
  - id: usb_audio_id
    speaker:
      sample_rate: 48000
      bits_per_sample: 16bit
      num_channels: 2
    stream_idle_timeout: 30s
```

`usb_bluetooth`'s `firmware` is a local file next to the config (here the Realtek RTL8851BU firmware). The examples
are from the Waveshare ESP32-P4 Pi board's config. `external_components` replaces components only; the ESPHome core
of the build host stays as installed. With the other topics together use the combined branch
`github://p1ngb4ck/esphome@topics-combined`.
