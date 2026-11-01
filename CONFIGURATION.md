# Configuration reference

New options of ESPHome's network components; everything else is as documented on
[esphome.io](https://esphome.io/components/). The first steps are in [README.md](README.md).

## `network`

| Option | Default | Description |
|---|---|---|
| `priority` | - | Interfaces in order (`wifi`, `ethernet`, `usb_ethernet`, ...). The first connected one carries the default route and DNS. The order can be changed at runtime and is saved. |
| `on_active_interface` | - | ESP32, more than one interface: the active interface changed. |
| `name_from_nvs` | - | ESP32: name and friendly name from the internal NVS (one firmware for many devices): `namespace` (required), keys `name` (`name`), `friendly_name` (`friendly_name`), `name_suffix` / `friendly_name_suffix` (appended, `""`). Used by wifi, ethernet, mdns, api, captive_portal, improv_serial and web_server. |

When another interface's address change clears the DNS servers, those of the default interface are written back.

```yaml
network:
  priority:
    - ethernet
    - wifi
    - usb_ethernet
  on_active_interface:
    - logger.log: active interface changed
  name_from_nvs:
    namespace: device
```

## `wifi`

| Option | Default | Description |
|---|---|---|
| `saved_networks` | - | ESP32: room for 1-16 networks that can be added / removed at runtime (and are saved), beside the configured ones. |
| `on_scan_results` | - | A scan finished; the results are available to lambdas / UIs (a browse scan does not disconnect). |

## `ethernet`

No new options. At runtime: on / off and the address (DHCP or manual) are changeable and saved; the YAML
`manual_ip` becomes the NVS default; DHCP is not restarted without a link; the EMAC driver runs only while it has a
link and no interface of higher priority is connected.

## `improv_serial`

| Option | Default | Description |
|---|---|---|
| `identity` | - | ESP32: the RPCs 0x85 / 0x86 / 0x87 set name, friendly name and an API key, stored in the NVS `namespace` (required; pair with `network: name_from_nvs` of the same namespace). |

RPC 0x80 lists the interfaces in priority order and sets on / off, DHCP or a manual address.

## API or MQTT per entity

With `api:` and `mqtt:` both configured, each entity reaches Home Assistant one way:

| Option (`mqtt:`) | Default | Description |
|---|---|---|
| `publish_default` | `both` | `api`, `mqtt` or `both` for every entity not listed below. |
| `publish_api` / `publish_mqtt` / `publish_both` | - | Entity IDs that go only that way. |

The choice is changeable at runtime.

```yaml
mqtt:
  broker: mqtt.local
  publish_default: api
  publish_mqtt: [outdoor_temperature]
```

## `esp32_hosted` update sources

The `update` platform for the ESP32-C6 (or other co-processor) firmware takes a `type`:

| Type | Options |
|---|---|
| `embedded` | `path` (a file next to the YAML) or `url` (downloaded at build time), `sha256` (required). |
| `http` | `source` (a manifest URL), the usual `update_interval`. |
| `storage` | `source` (a file on a storage), `sha256`. |

Several entities can exist in one build; installs run as one non-blocking sequence.

```yaml
update:
  - platform: esp32_hosted
    name: C6 firmware
    type: http
    source: https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json
  - platform: esp32_hosted
    name: C6 firmware from SD
    type: storage
    source: /sdcard/firmware/c6.bin
```

## `boot_announce`

A receiver waiting for a device to come back (after a restart, an OTA update, a crash) gets the boot's facts at once:
`boot_announce.watch` sets a mark in RTC memory; after the next boot, once the network is up, the device sends a UDP
packet (through a `udp` component) and repeats it until the receiver acknowledges it. Needs `udp:`.

| Option | Description |
|---|---|
| `udp_id` | The `udp` component (its addresses and port). |

`boot_announce.watch` arms the announcement for the next boot.

```yaml
udp:
  id: boot_udp
  addresses: [192.168.1.20]
  port: 18512

boot_announce:
  udp_id: boot_udp
```
