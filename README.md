# ESPHome with the p1ngb4ck topic additions

> **Thank you first.** The list of people whose work this builds on has grown so large that it has its own file
> now - so the important information below is not hidden under all the attributions and credits:
> **[ATTRIBUTIONS.md](ATTRIBUTIONS.md)**.

`dev` is ESPHome's `dev` branch with every topic repository below merged in: storages and file access, USB host and
USB devices, Bluetooth, audio, camera and video, displays and touch, HID input, networking, apps for the ESP32-P4 and
more. Each topic repository holds one area, split into one branch per change, and documents its components.

## Getting started

Take the components you need from `dev`:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome@dev
    components: [storage, sd_storage]

storage:

sd_storage:
  clk_pin: GPIO43
  cmd_pin: GPIO44
  data0_pin: GPIO39
  data1_pin: GPIO40
  data2_pin: GPIO41
  data3_pin: GPIO42
```

Use your board's pins. `external_components` replaces components only; the ESPHome core of the build host stays as
installed, so use an ESPHome version close to the `dev` this branch is based on. Only one area needed? Take it from
its topic repository instead (`github://p1ngb4ck/esphome-<topic>@main`).

Each topic's README starts with the most important steps; its `CONFIGURATION.md` lists every option of every
component with examples for each variant.

## Topics

| Topic | What | Docs |
|---|---|---|
| [esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface) | `storage`: one interface for every storage, async file jobs and streams, file actions, exFAT, the web server's file API and file browser | [README](https://github.com/p1ngb4ck/esphome-storage-interface#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-storage-interface/blob/main/CONFIGURATION.md) |
| [esphome-storage-drivers](https://github.com/p1ngb4ck/esphome-storage-drivers) | SD card, NFS, FTP / FTPS / TFTP (client and server), S3, EEPROM / FRAM / MRAM / SPI flash, preferences on a storage, firmware updates from a storage, the embedded YAML | [README](https://github.com/p1ngb4ck/esphome-storage-drivers#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-storage-drivers/blob/main/CONFIGURATION.md) |
| [esphome-cert-store](https://github.com/p1ngb4ck/esphome-cert-store) | `cert_store`: CA and client certificates from files, storages, Home Assistant or HTTP for every TLS client | [README](https://github.com/p1ngb4ck/esphome-cert-store#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-cert-store/blob/main/CONFIGURATION.md) |
| [esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host) | `usb_host` rework: devices routed by class (plug in and it runs), both ESP32-P4 controllers, mode switching | [README](https://github.com/p1ngb4ck/esphome-usb-host#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-usb-host/blob/main/CONFIGURATION.md) |
| [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers) | USB mass storage, optical drives, HID, UVC cameras, Bluetooth dongles, USB audio, serial adapters, USB Ethernet | [README](https://github.com/p1ngb4ck/esphome-usb-drivers#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-usb-drivers/blob/main/CONFIGURATION.md) |
| [esphome-hid](https://github.com/p1ngb4ck/esphome-hid) | `hid`: keyboards, mice, gamepads, remotes for any transport; LVGL driven by them, key bindings, remote input | [README](https://github.com/p1ngb4ck/esphome-hid#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-hid/blob/main/CONFIGURATION.md) |
| [esphome-keyboard-layouts](https://github.com/p1ngb4ck/esphome-keyboard-layouts) | keyboard layouts per keyboard, from a storage, Home Assistant or HTTP | [README](https://github.com/p1ngb4ck/esphome-keyboard-layouts#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-keyboard-layouts/blob/main/CONFIGURATION.md) |
| [esphome-bluetooth](https://github.com/p1ngb4ck/esphome-bluetooth) | Bluetooth Classic, pairing and persistence, Classic and BLE HID, A2DP source, the ESP32-C6 offload | [README](https://github.com/p1ngb4ck/esphome-bluetooth#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-bluetooth/blob/main/CONFIGURATION.md) |
| [esphome-audio](https://github.com/p1ngb4ck/esphome-audio) | AAC, up to eight channels, runtime routing of outputs and microphones, echo cancellation, codec options | [README](https://github.com/p1ngb4ck/esphome-audio#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-audio/blob/main/CONFIGURATION.md) |
| [esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video) | HW JPEG / H.264, MIPI-CSI cameras, the display's video output, AVI player, screen capture, apps, stream receiver | [README](https://github.com/p1ngb4ck/esphome-esp-video#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-esp-video/blob/main/CONFIGURATION.md) |
| [esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display) | MIPI DSI options, video-only bridges, scanout, LVGL RGB888, touch drivers | [README](https://github.com/p1ngb4ck/esphome-mipi-display#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-mipi-display/blob/main/CONFIGURATION.md) |
| [esphome-rpi-7inch-display](https://github.com/p1ngb4ck/esphome-rpi-7inch-display) | the Raspberry Pi 7" Touch Display v1 on the ESP32-P4 | [README](https://github.com/p1ngb4ck/esphome-rpi-7inch-display#readme) |
| [esphome-dfrobot-dfr0550-display](https://github.com/p1ngb4ck/esphome-dfrobot-dfr0550-display) | the DFRobot DFR0550 5" DSI display on the ESP32-P4: FT5x06 firmware reads, I2C bus recovery | [README](https://github.com/p1ngb4ck/esphome-dfrobot-dfr0550-display#readme) |
| [esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform) | chip revision and flash detection, crash dumps, NVS tools, PSRAM speeds, boards | [README](https://github.com/p1ngb4ck/esphome-esp32-platform#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-esp32-platform/blob/main/CONFIGURATION.md) |
| [esphome-network](https://github.com/p1ngb4ck/esphome-network) | interface priority at runtime, saved WiFi networks, Ethernet at runtime, Improv network setup, API / MQTT per entity, boot announce | [README](https://github.com/p1ngb4ck/esphome-network#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-network/blob/main/CONFIGURATION.md) |
| [esphome-uart-tcp](https://github.com/p1ngb4ck/esphome-uart-tcp) | UART over the network: virtual UARTs, encryption, the log as a UART, forwards, RFC 2217 | [README](https://github.com/p1ngb4ck/esphome-uart-tcp#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-uart-tcp/blob/main/CONFIGURATION.md) |
| [esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link) | the link to the esphome-media-relay Home Assistant integration, fixed-size containers | [README](https://github.com/p1ngb4ck/esphome-relay-link#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-relay-link/blob/main/CONFIGURATION.md) |
| [esphome-runtime-language](https://github.com/p1ngb4ck/esphome-runtime-language) | the UI language picked at runtime: LVGL labels and web server names | [README](https://github.com/p1ngb4ck/esphome-runtime-language#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-runtime-language/blob/main/CONFIGURATION.md) |
| [esphome-emulator](https://github.com/p1ngb4ck/esphome-emulator) | game emulator app for the ESP32-P4 | [README](https://github.com/p1ngb4ck/esphome-emulator#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-emulator/blob/main/CONFIGURATION.md) |
| [esphome-remote-desktop](https://github.com/p1ngb4ck/esphome-remote-desktop) | VNC and SPICE remote desktop app | [README](https://github.com/p1ngb4ck/esphome-remote-desktop#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-remote-desktop/blob/main/CONFIGURATION.md) |
| [esphome-browser](https://github.com/p1ngb4ck/esphome-browser) | web browser app through a remote browser server | [README](https://github.com/p1ngb4ck/esphome-browser#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-browser/blob/main/CONFIGURATION.md) |
| [esphome-phone](https://github.com/p1ngb4ck/esphome-phone) | SIP phone | [README](https://github.com/p1ngb4ck/esphome-phone#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-phone/blob/main/CONFIGURATION.md) |
| [esphome-messenger](https://github.com/p1ngb4ck/esphome-messenger) | messenger app with a Telegram client | [README](https://github.com/p1ngb4ck/esphome-messenger#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-messenger/blob/main/CONFIGURATION.md) |
| [esphome-pool](https://github.com/p1ngb4ck/esphome-pool) | smaller additions: ADC sensors (ACS712, ZMPT101B, ADS1115), select, S/PDIF, LVGL fixes | [README](https://github.com/p1ngb4ck/esphome-pool#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-pool/blob/main/CONFIGURATION.md) |
| [esphome-package-manager](https://github.com/p1ngb4ck/esphome-package-manager) | packages built with the firmware (`.ipk`), installed, enabled and removed at runtime from a storage or from signed online repositories with one package directory per firmware; HA API, MQTT, web and YAML | [README](https://github.com/p1ngb4ck/esphome-package-manager#readme) - [Configuration](https://github.com/p1ngb4ck/esphome-package-manager/blob/main/CONFIGURATION.md) |

## License

- ESPHome's own files, also where the topics change them: the [ESPHome License](LICENSE) (MIT for the Python code
  and everything else, GPLv3 for the C++ runtime files).
- New files written for the topic repositories (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).
- Third-party code keeps the licence its authors chose; [ATTRIBUTIONS.md](ATTRIBUTIONS.md) lists it with its
  licence. Some emulator cores (fMSX, Snes9x) do not allow commercial use.

## ESPHome

Everything here is ESPHome: [Documentation](https://esphome.io) -
[Issues](https://github.com/esphome/esphome/issues) - [Feature requests](https://github.com/orgs/esphome/discussions).
Problems with the additions of this fork belong to this repository's issues, not ESPHome's.

[![ESPHome - A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/esphome.png)](https://www.openhomefoundation.org/)
