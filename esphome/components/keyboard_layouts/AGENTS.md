# keyboard_layouts: guide for AI agents

Layouts for `hid` inputs at runtime (read `esphome/components/hid/AGENTS.md` first; `hid::parse_layout()` reads the
`.kbl` format).

- `keyboard_layouts.*`: `KeyboardLayouts` - the keyboard table (device key -> layout, saved in the preferences), the
  list of available layouts (union of the sources), loading one layout, the device select and the layout sensor.
- Sources: `directory` through the storage interface (listing via `list_dir`, files through worker streams), Home
  Assistant through `relay_link` and plain HTTP sources through `relay_link::http_at()`
  (`USE_KEYBOARD_LAYOUTS_LINK`). Network requests run on the relay link's task; completion flags (atomics) are
  picked up on the main loop, which applies the result.
- The configured `keyboards` are matched to connected HID devices by their device key (with the serial number), so
  two keyboards of the same model keep separate layouts.
- Layout data is parsed into the input's layout and the download buffer is freed afterwards; the list of names is
  bounded.
- The preference key name is fixed (changing it loses users' saved assignments).
