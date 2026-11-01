# usb_uart: guide for AI agents

USB serial adapters as `uart` buses. Reworked from upstream: one client for every adapter, chip drivers by type.
Read `esphome/components/usb_host/AGENTS.md` first.

- `__init__.py`: the type table (`uart_types`: name, default VID / PID, driver, channel count, maximum baud). A new
  chip is a new row plus, if its protocol differs, a driver file; only drivers of configured types are compiled
  (`FILTER_SOURCE_FILES`).
- `usb_uart.*`: `USBUartClient`, `USBUartChannel` (a `UARTComponent`); adapters without an entry get their type
  from VID / PID, else CDC ACM.
- Drivers: `ch34x.cpp`, `ch934x.cpp` (CH9344 / CH348: channels multiplexed over shared endpoints, a shared TX
  queue, paced channel init, DTR / RTS), `cp210x.cpp`, `ft23xx.cpp`, `pl2303.cpp`; CDC ACM in `usb_uart.cpp`.

Rules:
- The RX buffer of a channel is allocated when the device connects and freed after it is drained on disconnect;
  receive chunk sizes follow the endpoint's packet size.
- Line settings (baud, data bits, parity, stop bits) can change at runtime (`uart` runtime line settings,
  esphome-uart-tcp) and are applied with the chip's own requests.
- Transfer callbacks run on the USB task and only move bytes into the ring buffer; consumers read on the main loop.
