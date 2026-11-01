# usb_host: guide for AI agents

The USB host and the base class of every USB driver (`USBClient`). The drivers live in esphome-usb-drivers; a
change here is a change for all of them.

## Files
- `usb_host.h`: `USBHost`, `USBClient`, `UsbDevice`, `TransferRequest`, `IsocStream`, `UsbMatch`, `ModeSwitch`.
- `usb_host_component.cpp`: the host - the shared USB task, enumeration, routing, mode switching, hubs, the host
  channel budget per controller.
- `usb_host_client.cpp`: the client side - claiming interfaces, the transfer engine (bulk, interrupt, control,
  isochronous streams), connect / disconnect.
- `idf_usb_patch.py`: patches applied to a copy of the espressif/usb component at build time (per-port FIFO bias,
  `fsls_only`, external hub port requests, root port reset recovery). The later patches anchor on code the FIFO bias
  patch adds; keep them in order and keep the anchors exact.
- `__init__.py`: options, `usb_device_schema()` (the `vid` / `pid` / manufacturer / product / `blacklist` options
  every driver gets), `require_*()` functions through which drivers declare the transfer types they need (the union
  becomes defines, so unused transfer code is not built).

## Routing (the core idea)
A device is routed once, when it is enumerated: a driver whose configured filters name it (`NAMED`), then a driver
whose built-in table names it (`VENDOR`, `refine_match_()`), then the drivers of its interface classes. A driver gets
the interfaces it matched (`interface_routed()`), not the whole device; several drivers can share one device. Drivers
never enumerate on their own and never take devices by polling. A driver that serves several devices says so with
`accepts_more_devices_()`.

## Threading
- One shared USB task (`usb_task_fn`) runs the USB host library events for all clients; a client is registered with
  the library only while a device is bound to it.
- Transfer callbacks run on the USB task. They copy what they need into the client's own (PSRAM) buffers or queues
  and hand events to the main loop; no ESPHome entity, storage or LVGL call from a transfer callback.
- `on_device_connected()` / `on_device_disconnected()` / `free_device_context()` are the lifecycle hooks; all memory a
  device needed is freed in `free_device_context()`.

## Memory and limits
- Transfer buffers are sized from `max_packet_size` and allocated per client from the pool of
  `max_transfer_requests`; a request larger than one buffer is rejected.
- Host channels are a hardware limit per controller (the budget); isochronous streams and interrupt endpoints count
  against it. A driver asks for what it uses and skips endpoints it does not need (`skipped_endpoints_()`).
- Large driver buffers go to PSRAM unless the hardware needs DMA-capable internal RAM.

## ESP32-P4
Two controllers: high-speed (UTMI PHY) and full-speed (FS PHY on `fs_pins`). `controllers` / `dual_host` select them;
`tinyusb` can take the other one. The FS PHY swap (`24_25`) also moves USB-Serial-JTAG.
