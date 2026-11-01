# esphome-uart-tcp

UARTs over the network for ESPHome: `uart_tcp` puts a device's UART on a TCP port (as server or client), `tcp_uart`
gives the device a UART whose other end is a TCP connection, `noise` encrypts the stream between two devices, and on
top: virtual UARTs (the device's own log as a UART), several connections per port, UART forwards configured at
runtime from a file, RFC 2217 (remote serial ports with line settings and DTR / RTS), and a log ring for viewers.

## Getting started

A sensor's UART reachable on TCP port 6638:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-uart-tcp@main
    components: [uart, uart_tcp, socket]

uart:
  - id: meter_uart
    tx_pin: GPIO17
    rx_pin: GPIO18
    baud_rate: 115200

uart_tcp:
  - role: server
    uart_id: meter_uart
    port: 6638
```

The device's log on TCP port 4000, with the boot log kept for the first client:

```yaml
logger:
  uart:
    id: log_uart

uart_tcp:
  - role: server
    uart_id: log_uart
    port: 4000
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option - roles, encryption and runtime keys, RFC 2217, forwards and
their settings file, `tcp_uart`, the log as a UART, the log ring, runtime line settings.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

`uart_tcp`, `tcp_uart`, `noise` and `socket` are upstream components; the `pr/<number>-...` branches are Bascht74's
open upstream pull requests, the others are additions on top.

| Branch | Upstream PR | Needs |
|---|---|---|
| `pr/20097-tcp-idle-close` | [#20097](https://github.com/esphome/esphome/pull/20097) Close a quiet TCP connection | - |
| `pr/20482-noise-stream` | [#20482](https://github.com/esphome/esphome/pull/20482) An encrypted byte stream between two devices | `pr/20097-tcp-idle-close` |
| `pr/20483-tcp-uart-noise` | [#20483](https://github.com/esphome/esphome/pull/20483) Encrypt tcp_uart with the noise stream | `pr/20482-noise-stream` |
| `pr/20484-uart-tcp-noise` | [#20484](https://github.com/esphome/esphome/pull/20484) Encrypt uart_tcp with the noise stream | `pr/20483-tcp-uart-noise` |
| `pr/20508-noise-runtime-key` | [#20508](https://github.com/esphome/esphome/pull/20508) Set the encryption key at runtime | `pr/20484-uart-tcp-noise` |
| `pr/20198-uart-virtual-base` | [#20198](https://github.com/esphome/esphome/pull/20198) A virtual UART base | `pr/uart-debugger-prefix-settings` |
| `pr/20476-uart-claim-exclusive` | [#20476](https://github.com/esphome/esphome/pull/20476) Share uart_tcp's exclusive UART claim | - |
| `pr/21039-socket-several-links` | [#21039](https://github.com/esphome/esphome/pull/21039) Let TcpListener accept into several links | - |
| `pr/21040-tcp-uart-several-connections` | [#21040](https://github.com/esphome/esphome/pull/21040) Accept several connections on one port | `pr/21039-socket-several-links` |

Additions, stacked in this order on all of the above and `pr/uart-runtime-line-settings` (each contains the one
before it):

| Branch | Change |
|---|---|
| `pr/uart-debugger-prefix-settings` | uart: the debugger's prefix through the debug callback on every platform, `debug_add_uart_settings` |
| `pr/uart-runtime-line-settings` | uart: line settings settable at runtime and saved (ESP32, ESP8266) |
| `pr/noise-shared-key` | noise: a stream can run under another stream's key |
| `pr/uart-tcp-runtime-settings` | uart_tcp: runtime settings, saved |
| `pr/uart-virtual-ring` | uart: the virtual UART ring in PSRAM, re-allocatable, reader-connected hook |
| `pr/logger-virtual-uart` | logger: the log as a virtual UART (`logger: uart:`) |
| `pr/uart-tcp-virtual-peer` | uart_tcp: a virtual UART is told when the peer comes and goes |
| `pr/uart-tcp-forwards` | uart_tcp `role: forwards`: UARTs forwarded at runtime from a settings file |
| `pr/socket-peer-address` | socket: `TcpClientLink::peer_address()` |
| `pr/uart-tcp-logger-getters` | uart_tcp, logger: connection and buffer getters |
| `pr/uart-tcp-change-callbacks` | uart_tcp: connection and forwards change callbacks |
| `pr/socket-loop-interval` | socket, noise: `get_loop_interval()` in socket |
| `pr/uart-tcp-logger-psram` | uart_tcp, logger: objects in PSRAM, no callback vectors, the UART list in PSRAM |
| `pr/logger-log-ring` | logger `log_ring_size`: the recent lines in a PSRAM ring for viewers |
| `pr/uart-tcp-rfc2217` | uart_tcp `protocol: rfc2217`: RFC 2217 as server and client; line settings from the peer, DTR / RTS to a usb_uart channel |

The forwards' settings file lives on a storage
([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)). DTR / RTS forwarding needs
`usb_uart` ([esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers)).

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

This topic is built on [Bascht74](https://github.com/Bascht74)'s work: the virtual UART base, the exclusive UART
claim, the encrypted noise stream for `tcp_uart` / `uart_tcp`, the runtime key, the idle close, and several links
and connections per port. Thank you for a great collaboration and for all those pull requests. Everyone this project
builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
