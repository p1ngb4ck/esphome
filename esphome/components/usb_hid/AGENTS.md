# usb_hid: guide for AI agents

USB keyboards, mice, gamepads and remotes as inputs of the `hid` component (report parsing, layouts, events live
in `hid`; read `esphome/components/hid/AGENTS.md` and `esphome/components/usb_host/AGENTS.md`).

- `usb_hid.*`: `USBHIDClient`, any number of devices; per device the report descriptor is read and handed to `hid`,
  interrupt IN transfers deliver reports, output reports (keyboard LEDs) go out by control transfer.
- `xbox.*`: Xbox controllers (vendor protocol, no HID class) - routed through the vendor table.
- `mce.*`: Media Center remotes and similar devices with quirks.
- `raw_driver.h`: `raw:` devices - reports passed through as bytes, buttons by byte / mask, short / long press.
- `hid_types.h`: descriptor and usage constants.

Rules:
- Report callbacks run on the USB task: copy the report and hand it to the main loop; key / pointer events fire on
  the main loop.
- Per-device state is allocated on connect and freed on disconnect; the device key includes the serial number, so
  two identical devices are told apart.
