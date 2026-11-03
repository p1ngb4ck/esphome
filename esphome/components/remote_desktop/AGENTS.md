# remote_desktop: guide for AI agents

VNC and SPICE client app (ESP32-P4).

- `rfb.cpp`: RFB (VNC) - handshake, security (VNC auth with `vnc_des.*`, the DES that VNC authentication requires),
  encodings (Tight JPEG through the hardware JPEG decoder, raw / copy rectangles), cursor, input events.
- `spice.cpp`: SPICE - main / display / inputs / cursor / playback channels, MJPEG streams from the guest's
  spice-streaming-agent and JPEG images through the hardware decoder, sound to the speaker, usbredir.
- `connections.cpp`: saved connections in the preferences or a connections file (storage worker).
- `remote_desktop.*`: the session (connect, probe with `protocol: auto`, disconnect), drawing into a page canvas
  through the video output's page calls, input from HID / touch / key bindings, the close gesture, `is_connecting()` /
  `failed()` / `session()` for UIs and the browser app.
- `select/`: USB redirection slots (up to 4): a connected USB device is handed over to the remote machine
  (usbredir with SPICE, USB/IP with VNC) through `usb_host`.

Rules:
- Network I/O runs on the session's own task; drawing goes through the video output's page calls; LVGL objects are
  touched on the main loop only.
- Decode buffers belong to the session (PSRAM) and are freed when it ends.
- `browser` (esphome-browser) owns a session of its own through this component; both use the same code path.
