# esphome-network

Networking changes for ESPHome: several interfaces (WiFi, Ethernet, USB Ethernet) with a priority order that can be
changed at runtime and decides the default route, a WiFi network list changeable at runtime with scan results for a
UI, Ethernet settings at runtime, the device name from the NVS (one firmware for many devices), Improv serial setup
of interfaces and identity, which way each entity reaches Home Assistant (API, MQTT or both), ESP32-C6 firmware
updates for `esp32_hosted` from several sources, and `boot_announce` - a packet to a waiting host when the device is back after a restart.

## Getting started

WiFi and Ethernet, Ethernet preferred while it has a link:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-network@main
    components: [network, wifi, ethernet, mdns, api]

network:
  priority:
    - ethernet
    - wifi

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  saved_networks: 8

ethernet:
  type: IP101
  mdc_pin: GPIO31
  mdio_pin: GPIO52
  clk:
    pin: GPIO50
    mode: CLK_EXT_IN
  phy_addr: 1
  power_pin: GPIO51
```

Use your board's PHY and pins.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every new option - interface priority and its trigger, saved networks and
scan results, the name from the NVS, Improv identity, API / MQTT per entity, `esp32_hosted` update sources,
`boot_announce`.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/network-priority` | `network` `priority` changeable at runtime and saved, `on_active_interface`, the default route on the first connected interface in that order | ESP32 | - |
| `pr/network-restore-default-dns` | the default interface's DNS servers written back when another interface's address change cleared them | ESP32 | `pr/network-priority` |
| `pr/network-usb-ethernet` | usb_ethernet as a third arbitrated interface (network, mdns) | ESP32 with USB host | `pr/network-restore-default-dns` |
| `pr/network-name-from-nvs` | `name_from_nvs`: device name and friendly name from the NVS for every network component | ESP32 | `pr/network-priority` |
| `pr/network-wifi-idf61` | ESP-IDF 6.1: options removed there skipped (network, wifi) | ESP32 | - |
| `pr/network-nvs-ready` | wifi and openthread wait for `nvs_ready` | ESP32 | `pr/esp32-nvs-ready` |
| `pr/wifi-saved-networks` | `saved_networks`: a network list changeable at runtime, browse scan, `on_scan_results` | ESP32 | - |
| `pr/wifi-startup-done-callback` | `add_on_startup_done_callback` / `is_startup_done`: once per boot | ESP32 | - |
| `pr/wifi-ap-state-callback` | `add_on_ap_state_callback`: the access point up / down | ESP32 | `pr/wifi-startup-done-callback` |
| `pr/ethernet-runtime` | Ethernet address and on / off at runtime and saved, the YAML `manual_ip` as an NVS default, no DHCP restart without a link, the EMAC driver only while it has a link and no higher-priority interface is connected | ESP32, P4, S31 (EMAC) | `pr/esp32-nvs-defaults` |
| `pr/network-tasks-loop-core` | mqtt and ethernet tasks pinned to `LOOP_TASK_CORE` | ESP32 | `pr/esp32-loop-task-core` |
| `pr/improv-serial-network` | Improv serial RPC 0x80: interfaces in priority order, on / off, DHCP or a manual address | as improv_serial | - |
| `pr/improv-serial-identity` | Improv serial RPCs 0x85 / 0x86 / 0x87: name, friendly name, an API key stored | ESP32 | `pr/improv-serial-network`, `pr/network-name-from-nvs` |
| `pr/mqtt-api-publish-way` | which way each entity reaches Home Assistant: `publish_default`, `publish_api` / `publish_mqtt` / `publish_both`, changeable at runtime | as mqtt / api | - |
| `pr/mqtt-events-free` | inbound MQTT events give their topic / payload memory back when released | ESP32 | - |
| `pr/mqtt-tls-sha512` | MQTT over TLS keeps SHA-384 / SHA-512 | ESP32 | `pr/esp32-cert-bundle-sha512` |
| `pr/esp32-hosted-update-sources` | `esp32_hosted` update: `embedded`, `http` and `storage` sources per entity, one non-blocking install sequence, `sha256` | P4, H2 (hosts) | `pr/storage` |
| `pr/boot-announce` | `boot_announce`: after an armed restart, a UDP packet once the network is up, repeated until acknowledged | ESP32 | `pr/network-priority` |

`pr/esp32-*` are in [esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform), `pr/storage` in
[esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface); `usb_ethernet` itself is in
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
