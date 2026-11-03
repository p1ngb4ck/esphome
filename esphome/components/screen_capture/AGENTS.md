# screen_capture: guide for AI agents

Screenshots and screencasts of what LVGL shows.

- `screen_capture.*`: one capture - the frame read from the display (or LVGL's buffer), JPEG through the hardware
  encoder, written through the storage worker; screencasts as AVI (MJPEG + sound from a microphone source and / or
  what the device plays), locally or to the relay link.
- `screen_capture_component.*`: the YAML component, triggers, the action.
- Captures never block the main loop; frame reads happen at frame end so the picture is consistent.

Video rules (all video components):
- Never touch LVGL while LVGL is paused or the video output writes the frame buffers directly.
- The device never rescales or resizes at runtime beyond what the hardware (PPA, JPEG decoder) does in one pass;
  frames are not dropped to keep up - fix the pipeline instead.
- Frame and stream buffers live in PSRAM, are allocated when playback / capture starts and freed when it stops.
- Decoding and encoding run on the hardware engines asynchronously; the main loop only starts jobs and takes
  completions.
