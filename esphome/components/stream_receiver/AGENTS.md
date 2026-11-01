# stream_receiver: guide for AI agents

A media player for a stream from Home Assistant (a desktop or browser rendered on the server): screen rectangles
into an LVGL canvas through the video output's page mode, sound to a speaker, touches back, AVI streams handed to
an `avi` player, a channel list in an LVGL dropdown.

- `media_player/stream_media_player.*`: two tasks - `stream_net` (the TCP receiver) and `stream_draw` (rectangles
  through the hardware JPEG decoder or copied raw into the page) - the output modes (canvas / full screen), channel
  actions.
- The tasks fill PSRAM buffers and draw through the video output's page calls, never through LVGL objects.

Video rules (all video components):
- Never touch LVGL while LVGL is paused or the video output writes the frame buffers directly.
- The device never rescales or resizes at runtime beyond what the hardware (PPA, JPEG decoder) does in one pass;
  frames are not dropped to keep up - fix the pipeline instead.
- Frame and stream buffers live in PSRAM, are allocated when playback / capture starts and freed when it stops.
- Decoding and encoding run on the hardware engines asynchronously; the main loop only starts jobs and takes
  completions.
