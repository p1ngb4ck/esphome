# Configuration reference

The first steps are in [README.md](README.md).

## `uart_tcp`

A UART of the device on a TCP connection. A list; `role` selects the kind.

| Option | Roles | Default | Description |
|---|---|---|---|
| `role` | - | required | `server` (listens on `port`), `client` (connects to `host:port`), `forwards` (UARTs from a settings file). |
| `uart_id` | server, client | required | The UART (also a virtual UART such as the log). |
| `port` | server, client | required | TCP port. |
| `host` | client | required | The server to connect to. |
| `allowed_ips` | server | all | Up to 255 IPv4 networks (`192.168.1.0/24`) allowed to connect. |
| `reconnect_interval` | server, client | `5s` | Client: retry interval. |
| `timeout` | server, client | `0s` | Close a connection after this long without data; `0s` = never. |
| `protocol` | server, client | `raw` | `raw` or `rfc2217` (Telnet COM-PORT-OPTION: the peer sets baud rate and line settings for the session; DTR / RTS go to a `usb_uart` channel's modem lines). |
| `encryption` | server, client | - | The noise stream: `key` (32 bytes, base64). Without `key` the key is set at runtime (`uart_tcp.set_encryption_key`). Both ends need the same key; it must differ from the API and OTA keys. |
| `runners` | forwards | `4` | Forwards running at once, 2-12. |
| `settings_file` | forwards | `/sdcard/serial_ports.txt` | The settings file on a storage (needs `storage:`). Only one `forwards` entry. |

```yaml
uart_tcp:
  - id: meter_link
    role: server
    uart_id: meter_uart
    port: 6638
    allowed_ips: [192.168.1.0/24]
    encryption:
      key: !secret uart_link_key
  - id: remote_port
    role: client
    uart_id: bridge_uart
    host: 192.168.1.30
    port: 6638
    protocol: rfc2217
  - id: serial_forwards
    role: forwards
    runners: 4
    settings_file: /sdcard/serial_ports.txt
```

Settings are changeable at runtime and saved (role, port, host, protocol, ...); the forwards' file is read through
the storage worker, and changes to it are written back the same way.

## `tcp_uart`

A UART whose other end is a TCP connection - for components that talk to a device that sits on the network.

| Option | Default | Description |
|---|---|---|
| `role` | `client` | `client` (`host` required) or `server`. |
| `port` | required | TCP port. |
| `baud_rate`, `data_bits`, `parity`, `stop_bits` | `9600`, `8`, `NONE`, `1` | Reported line settings. |
| `timeout` | `0s` | Close a quiet connection after this long. |
| `encryption` | - | The noise stream, as above. |
| `allowed_ips` | all | Server: allowed IPv4 networks. |
| `max_connections` | `1` | Server: connections at once, 1-8. |
| `turn_timeout` | - | Server with 2+ connections: a connection's turn ends after this long without its reply. |

```yaml
tcp_uart:
  - id: modbus_link
    role: client
    host: 192.168.1.40
    port: 502

modbus:
  uart_id: modbus_link
```

## `logger`

| Option | Default | Description |
|---|---|---|
| `uart` | - | The log as a virtual UART (for `uart_tcp` and every UART consumer): `id`, `boot_buffer_size` (`128kB`, PSRAM, `0` = no boot log), `live_buffer_size` (`16kB`), `boot_hold` (`10s`, at most 60 s: how long the boot log waits for a reader), `level`. |
| `log_ring_size` | `0` | Keep the recent log lines in a PSRAM ring for viewers (`0` or 4 KB - 1 MB). |

```yaml
logger:
  level: DEBUG
  log_ring_size: 16kB
  uart:
    id: log_uart
    boot_buffer_size: 128kB
    boot_hold: 10s
```

## `uart`

| Option | Default | Description |
|---|---|---|
| `debug.debug_add_uart_settings` | `false` | The line settings (`baud:data bits:parity:stop bits`) in every debug line. |

Line settings of every UART can be changed at runtime and are saved (ESP32, ESP8266); RFC 2217 and UIs use that.

## Actions

| Action | Description |
|---|---|
| `uart_tcp.set_encryption_key` / `tcp_uart.set_encryption_key` | `id`, `key` (templatable): the key of an entry configured without one. |
