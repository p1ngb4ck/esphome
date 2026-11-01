# usb_uvc: guide for AI agents

USB video class cameras as `camera` entities. Read `esphome/components/usb_host/AGENTS.md` (isochronous streams,
channel budget) first.

- `usb_uvc.*`: `UVCClient` (any number of cameras) and `UvcCamera`. Probe / commit of format, resolution and frame
  interval, the alternate setting with the smallest bandwidth that fits, isochronous IN streams (`num_urbs` in
  flight), payload headers parsed into frames.
- `uvc_types.h`: UVC descriptors and controls.
- Frames: MJPEG frames go to the camera's encoder (`jpeg` passes them through, `h264` re-encodes on the ESP32-P4)
  and, with `lvgl_id`, to the display's video output (esphome-esp-video), where the hardware JPEG decoder writes
  them into the scanout.

Rules:
- Stream packets arrive on the USB task; frames are assembled into PSRAM buffers and handed on without copies on the
  main loop.
- Streams start on request and return to `StreamState::IDLE` when stopped; keep stream buffers tied to the stream's
  lifetime.
- Isochronous bandwidth is the scarce resource: request the camera's real need, never the largest alternate setting.
