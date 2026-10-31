# esphome-uart-tcp

ESPHome `dev` plus the UART-over-network changes of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome)
`testing_dev`: the virtual UART, the exclusive UART claim, the encrypted noise stream for `tcp_uart` / `uart_tcp`,
several links per listener and several connections per port, `uart_tcp` runtime settings and forwards, the log as a
virtual UART and the log ring. One branch per change; `main` contains every branch. Base: ESPHome `dev` at
`1f70f86b47`. The code is taken over unchanged from `testing_dev`: the lines of uart, uart_tcp, tcp_uart, noise,
socket and logger assigned by `git blame`; `main` equals `testing_dev` for all of them.

`uart_tcp`, `tcp_uart`, `noise` and `socket` are upstream components; the branches `pr/<number>-...` are the open
upstream pull requests of Bascht74 as `testing_dev` carries them, the others are this fork's own changes on top.

## Branches

Upstream pull requests (Bascht74), as merged into `testing_dev`:

| Branch | Upstream PR | Needs |
|---|---|---|
| `pr/20097-tcp-idle-close` | [#20097](https://github.com/esphome/esphome/pull/20097) [tcp_uart][uart_tcp] Close a quiet TCP connection | - |
| `pr/20482-noise-stream` | [#20482](https://github.com/esphome/esphome/pull/20482) [noise] Add an encrypted byte stream between two devices | `pr/20097-tcp-idle-close` |
| `pr/20483-tcp-uart-noise` | [#20483](https://github.com/esphome/esphome/pull/20483) [tcp_uart] Encrypt tcp_uart with the noise stream | `pr/20482-noise-stream` |
| `pr/20484-uart-tcp-noise` | [#20484](https://github.com/esphome/esphome/pull/20484) [uart_tcp] Encrypt uart_tcp with the noise stream | `pr/20483-tcp-uart-noise` |
| `pr/20508-noise-runtime-key` | [#20508](https://github.com/esphome/esphome/pull/20508) [noise][tcp_uart][uart_tcp] Set the encryption key at runtime | `pr/20484-uart-tcp-noise` |
| `pr/20198-uart-virtual-base` | [#20198](https://github.com/esphome/esphome/pull/20198) [uart] Add a virtual UART base | `pr/uart-debugger-prefix-settings` |
| `pr/20476-uart-claim-exclusive` | [#20476](https://github.com/esphome/esphome/pull/20476) [uart] Share uart_tcp's exclusive UART claim | - |
| `pr/21039-socket-several-links` | [#21039](https://github.com/esphome/esphome/pull/21039) [socket] Let TcpListener accept into several links | - |
| `pr/21040-tcp-uart-several-connections` | [#21040](https://github.com/esphome/esphome/pull/21040) [tcp_uart] Accept several connections on one port | `pr/21039-socket-several-links` |

This fork's own changes, stacked in commit order on all of the above and `pr/uart-runtime-line-settings`:

| Branch | Change |
|---|---|
| `pr/uart-debugger-prefix-settings` | uart: the debugger's prefix through the debug callback on every platform, `debug_add_uart_settings` (the uart part of `pr/uart-debug-add-uart-settings` in esphome-usb-drivers); the virtual UART base uses this callback |
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
| `pr/socket-loop-interval` | socket, noise: `get_loop_interval()` in socket, not from core |
| `pr/uart-tcp-logger-psram` | uart_tcp, logger: objects in PSRAM, no callback vectors, the UART list in PSRAM |
| `pr/logger-log-ring` | logger `log_ring_size`: the recent lines in a PSRAM ring for viewers |

Each own branch contains the one before it. `uart_tcp` forwards' settings file lives on a storage
([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)).

## Contributions

- [Bascht74](https://github.com/Bascht74): the virtual UART base, the exclusive UART claim, the noise stream and its
  encryption of `tcp_uart` / `uart_tcp`, the runtime key, the idle close, several links and connections per port
  (upstream PRs #20097, #20198, #20476, #20482, #20483, #20484, #20508, #21039, #21040).

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-uart-tcp@main
    components: [uart, uart_tcp, tcp_uart, noise, socket, logger]

logger:
  level: DEBUG
  log_ring_size: 16kB
  uart:
    id: log_uart
    boot_buffer_size: 128kB
    live_buffer_size: 16kB
    boot_hold: 10s

uart_tcp:
  - id: log_tcp
    uart_id: log_uart
    role: server
    port: 4000
  - id: serial_forwards
    role: forwards
    runners: 4
    settings_file: /sdcard/serial_ports.txt
```

The log goes out on TCP port 4000; `serial_forwards` forwards UARTs named in the settings file. From the Waveshare
ESP32-P4 Pi board's config. `external_components` replaces components only; the ESPHome core of the build host
stays as installed. With the other topics together use the combined branch
`github://p1ngb4ck/esphome@topics-combined`.
