# esphome-usb-drivers

USB device drivers for ESPHome on the reworked `usb_host`
([esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host)): USB drives and optical drives as storages,
keyboards / mice / gamepads / remotes, UVC cameras, Bluetooth dongles, USB sound cards, USB serial adapters and USB
Ethernet adapters. Every driver takes its devices by class - plug a device in and it works, any number at once; `vid`
/ `pid` only pin one particular device to one particular entry.

## Getting started

A USB stick and a USB keyboard on an ESP32-S3 or ESP32-P4:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-usb-drivers@main
    components: [esp32, usb_host, storage, usb_storage, usb_hid, hid]

usb_host:
  enable_hubs: true

storage:

usb_storage:
  devices:
    - mount_path: /usb

usb_hid:
  - id: keyboard
    on_text:
      - logger.log:
          format: "typed '%s'"
          args: [text.c_str()]
```

A USB sound card as a speaker:

```yaml
usb_audio:
  - id: usb_sound

speaker:
  - platform: usb_audio
    usb_audio_id: usb_sound
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option of every driver with examples - `usb_storage`, `usb_cdrom`,
`usb_hid` (raw reports, buttons), `usb_uvc` (formats, LVGL output, H.264), `usb_bluetooth` (firmware, Bluedroid,
offload), `usb_audio` (speaker, microphone, media player, quirks), `usb_uart` (every adapter type, multi-channel
chips), `usb_ethernet`.

The options every USB driver shares (`vid`, `pid`, `blacklist`) are in esphome-usb-host's
[CONFIGURATION.md](https://github.com/p1ngb4ck/esphome-usb-host/blob/main/CONFIGURATION.md); keyboard layouts and
the input events of `usb_hid` in [esphome-hid](https://github.com/p1ngb4ck/esphome-hid/blob/main/CONFIGURATION.md).

## Branches

One branch per driver or change; `main` contains every branch and esphome-usb-host's `main`. Base: ESPHome `dev` at
`1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/usb-storage` | `usb_storage`: USB mass storage (bulk-only, SCSI) on the storage interface, FAT / exFAT | S2, S3, S31, P4, H4 | `pr/storage`, `pr/usb-host-refactor` |
| `pr/usb-cdrom` | `usb_cdrom`: USB optical drives, ISO 9660 through the storage interface | S2, S3, S31, P4, H4 | `pr/usb-storage` |
| `pr/usb-hid` | `usb_hid`: keyboards, mice, gamepads, remotes, any number at once, through the `hid` component | S2, S3, S31, P4 | `pr/usb-host-refactor`, `pr/hid` |
| `pr/usb-uvc` | `usb_uvc`: USB video class cameras (MJPEG over isochronous streams), any number at once | S2, S3, S31, P4 | `pr/usb-host-refactor`, `pr/camera-encoder-h264` |
| `pr/usb-uvc-video-output` | `usb_uvc`: frames shown through the display's video output, the camera encoder option | P4 | `pr/usb-uvc`, `pr/camera-encoder-option`, `pr/display-video-output` |
| `pr/usb-bluetooth` | `usb_bluetooth`: Bluetooth dongles as HCI controller - Bluedroid on the dongle, firmware download | S2, S3, S31, P4 | `pr/usb-host-refactor`, `pr/esp32-ble-start-order` |
| `pr/usb-audio` | `usb_audio` rework: several devices, a format per device, streams open only while used, `feedback`, volume curve and quirks, up to 8 channels | S2, S3, S31, P4 | `pr/usb-host-refactor` |
| `pr/usb-audio-dac` | `usb_audio` `audio_dac` platform: the device's volume and mute | as `usb_audio` | `pr/usb-audio` |
| `pr/usb-audio-media-player` | `usb_audio` `media_player` platform | as `usb_audio` | `pr/usb-audio` |
| `pr/usb-uart` | `usb_uart` rework: one client for every serial adapter, RX buffer only while connected, runtime line settings | S2, S3, S31, P4, H4 | `pr/usb-host-refactor`, `pr/uart-runtime-line-settings` |
| `pr/usb-uart-ch934x` | `usb_uart` CH9344 / CH348: multiplexed channels, DTR / RTS | as `usb_uart` | `pr/usb-uart` |
| `pr/usb-ethernet` | `usb_ethernet`: CDC-ECM / CDC-NCM adapters as a network interface beside WiFi and Ethernet | S2, S3, S31, P4 | `pr/usb-host-refactor`, `pr/network-usb-ethernet` |
| `pr/uart-debug-add-uart-settings` | `uart` and `usb_uart` `debug_add_uart_settings`: the line settings in every debug line | all | `pr/usb-uart` |

Branches of other topics, contained as merges: `pr/storage`
([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)), `pr/usb-host-refactor`
([esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host)), `pr/hid`
([esphome-hid](https://github.com/p1ngb4ck/esphome-hid)), `pr/camera-encoder-h264`, `pr/camera-encoder-option`,
`pr/display-video-output` ([esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video)),
`pr/esp32-ble-start-order` ([esphome-bluetooth](https://github.com/p1ngb4ck/esphome-bluetooth)),
`pr/network-usb-ethernet` ([esphome-network](https://github.com/p1ngb4ck/esphome-network)),
`pr/uart-runtime-line-settings` ([esphome-uart-tcp](https://github.com/p1ngb4ck/esphome-uart-tcp)).

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

Thank you, [NonaSuomy](https://github.com/NonaSuomy), for parts of `usb_hid` and many of the ideas behind the USB
HID work. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
