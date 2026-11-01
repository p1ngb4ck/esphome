# uart_tcp: guide for AI agents

A UART on a TCP connection (server / client), forwards from a settings file, RFC 2217. Upstream component with this
topic's additions (esphome-uart-tcp).

- The UART is claimed exclusively (`uart` claim, Bascht74's upstream PR): nothing else reads it while a link runs.
- Sockets: every entry reserves its listen socket (the role can change at runtime), forwards reserve one per runner
  up to `MAX_RUNNERS` (12); `socket.consume_sockets()` keeps lwIP's socket budget correct. Never open sockets
  outside that budget.
- Encryption: the `noise` stream (`USE_NOISE_STREAM_RUNTIME_KEY` for keys set at runtime); keys must differ from
  the API and OTA keys (checked in codegen).
- RFC 2217 (`protocol: rfc2217`): Telnet COM-PORT-OPTION parsing in the stream; line settings apply to the UART for
  the session; DTR / RTS reach a `usb_uart` channel's modem control.
- Forwards (`uart_tcp_forwards.cpp`): the settings file is read and written through storage worker streams with the
  borrowed transfer buffer (the pattern for small settings files); runners start / stop at runtime.
- Virtual UARTs (the log, `logger: uart:`) are told when a peer connects and goes, so the boot log is replayed to the
  first reader.
- Objects and lists live in PSRAM; no callback vectors (fixed listener slots).
