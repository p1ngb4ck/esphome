# display (video output): guide for AI agents

The display topic's additions for video (esphome-esp-video); the scanout itself is in esphome-mipi-display.

- `video_output.*`: `display::VideoOutput` - the hardware JPEG decoder writes frames straight into the scanout
  buffer (`scanout_jpeg.*`), into 1-2 LVGL canvases that follow the frame size, or in page mode into a page of its
  own; capability report (formats, sizes) for players.
- Page draw calls (`page_rect`, `page_fill`, `page_pixels`, `page_copy`, `page_read`, `page_show`, `page_bpp`,
  `canvas_point`) for writers such as the remote desktop and the stream receiver.
- Rows (`open_rows`, `rows_begin` / `rows_end`): the panel's rows for a writer of its own (the emulator's PPA pass).
- `rect.*`: rectangles shared by the writers.
- One writer owns the output at a time (`open_*` / close); LVGL is paused while a writer draws full screen.

Video rules (all video components):
- Never touch LVGL while LVGL is paused or the video output writes the frame buffers directly.
- The device never rescales or resizes at runtime beyond what the hardware (PPA, JPEG decoder) does in one pass;
  frames are not dropped to keep up - fix the pipeline instead.
- Frame and stream buffers live in PSRAM, are allocated when playback / capture starts and freed when it stops.
- Decoding and encoding run on the hardware engines asynchronously; the main loop only starts jobs and takes
  completions.
