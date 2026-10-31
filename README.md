# esphome-network

ESPHome `dev` plus the network changes of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`:
interface priority at runtime, DNS restore, the device name from NVS, WiFi saved networks and callbacks, Ethernet
at runtime, Improv serial network setup and identity, the per-entity way to Home Assistant (API / MQTT), the
esp32_hosted update sources, boot_announce. One branch per change; `main` contains every branch. Base: ESPHome
`dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`: every line was assigned to the commit
that wrote it (`git blame`), each branch is `testing_dev` without the lines of the other branches. `main` equals
`testing_dev` for these components, except the lines of other topics (api: H.264 camera, BLE HID events;
esp32_hosted: Bluetooth offload peer handlers; web_server: i18n, select revisions).

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/network-priority` | `network` `priority` changeable at runtime and saved, `on_active_interface`, the default route on the first connected interface in that order | ESP32 | - |
| `pr/network-restore-default-dns` | `restore_default_dns`: the default interface's DNS servers written back when another interface's address change cleared them (wifi, ethernet) | ESP32 | `pr/network-priority` |
| `pr/network-usb-ethernet` | usb_ethernet as a third arbitrated interface (network, mdns) | ESP32 with USB host | `pr/network-restore-default-dns` |
| `pr/network-name-from-nvs` | `name_from_nvs`: device name and friendly name from NVS for wifi, ethernet, mdns, api, captive_portal, improv_serial, web_server | ESP32 | `pr/network-priority` |
| `pr/network-wifi-idf61` | ESP-IDF 6.1: options removed there skipped (network, wifi) | ESP32 | - |
| `pr/network-nvs-ready` | wifi and openthread wait for `nvs_ready` | ESP32 | `pr/esp32-nvs-ready` |
| `pr/wifi-saved-networks` | `saved_networks`: a network list changeable at runtime, browse scan, `on_scan_results` | ESP32 | - |
| `pr/wifi-startup-done-callback` | `add_on_startup_done_callback` / `is_startup_done`: once per boot, connected, the first attempt over, or WiFi disabled | ESP32 | - |
| `pr/wifi-ap-state-callback` | `add_on_ap_state_callback`: true when the access point is up, false when it goes | ESP32 | `pr/wifi-startup-done-callback` |
| `pr/ethernet-runtime` | Ethernet address and on / off at runtime and saved, the YAML `manual_ip` default in an NVS image, no DHCP restart without a link, the EMAC driver only while it has a link and no higher-priority interface is connected, other netifs' IP events ignored | ESP32, P4, S31 (EMAC) | `pr/esp32-nvs-defaults` |
| `pr/network-tasks-loop-core` | mqtt and ethernet tasks pinned to `LOOP_TASK_CORE` | ESP32 | `pr/esp32-loop-task-core` |
| `pr/improv-serial-network` | Improv serial RPC 0x80: interfaces in priority order, on / off, DHCP or a manual address | as improv_serial | - |
| `pr/improv-serial-identity` | Improv serial RPCs 0x85 / 0x86 / 0x87: name, friendly name, an API key stored | ESP32 | `pr/improv-serial-network`, `pr/network-name-from-nvs` |
| `pr/mqtt-api-publish-way` | which way each entity reaches Home Assistant: `publish_default` (`api`, `mqtt`, `both`), `publish_api` / `publish_mqtt` lists, changeable at runtime | as mqtt / api | - |
| `pr/mqtt-events-free` | inbound MQTT events give their topic / payload storage back when released | ESP32 | - |
| `pr/mqtt-tls-sha512` | MQTT over TLS keeps SHA-384 / SHA-512 | ESP32 | `pr/esp32-cert-bundle-sha512` |
| `pr/esp32-hosted-update-sources` | `esp32_hosted` update: embedded, http and storage as sources per entity, several in one build, one non-blocking install sequence, `sha256` | P4, H2 (hosts) | `pr/storage` |
| `pr/boot-announce` | `boot_announce`: a UDP packet to a host after every boot once the network is up, repeated until acknowledged | ESP32 | `pr/network-priority` |

`pr/esp32-*` are in [esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform), `pr/storage` in
[esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface). Two `testing_dev` changes have
no branch because upstream `dev` has them by now: `require_ipv4` / `require_ipv6` (#14526) and the esp32_hosted
sdkconfig skips in network / wifi (the esp32 part is `pr/esp32-idf61-hosted-sdkconfig`). `usb_ethernet` itself is
in [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-network@main
    components: [esp32, network, wifi, ethernet, mdns, api, mqtt, improv_serial, esp32_hosted, boot_announce]

network:
  priority:
    - wifi
    - ethernet
  on_active_interface:
    then:
      - script.execute: net_changed

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

update:
  - platform: esp32_hosted
    id: esp32_c6_update
    name: "C6 Update"
    type: http
    source: https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json
    update_interval: 6h

boot_announce:
  udp_id: boot_udp
```

The pins are those of the Waveshare ESP32-P4 Pi board. `external_components` replaces components only; the ESPHome
core of the build host stays as installed. With the other topics together use the combined branch
`github://p1ngb4ck/esphome@topics-combined`.
