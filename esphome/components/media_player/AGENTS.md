# media_player (options): guide for AI agents

Options every media player can take (esphome-esp-video `pr/media-player-options`):

- `pictures.*` (`MediaPictures`): pictures and slideshows into an `online_image`, folder listings through the storage
  worker, thumbnails, Home Assistant picture sources (`show_sources`), slideshow steps.
- `controls.*` (`MediaControls`): an LVGL control bar over the video rows, buttons, title, theming styles, hide timer;
  keys from the key bindings.
- `queue.*` (`MediaQueue`): play queue with repeat / shuffle, folder playlists (`play_folder`), Home Assistant items
  through the relay link.
- `extended_api_services.*`: API services (play with metadata, commands, volume, cover art into LVGL canvases) and a
  status text sensor.
- Each block is created only when configured; their lists live in PSRAM and are bounded (`max_entries`, 50
  thumbnails).

Video rules (all video components):
- Never touch LVGL while LVGL is paused or the video output writes the frame buffers directly.
- The device never rescales or resizes at runtime beyond what the hardware (PPA, JPEG decoder) does in one pass;
  frames are not dropped to keep up - fix the pipeline instead.
- Frame and stream buffers live in PSRAM, are allocated when playback / capture starts and freed when it stops.
- Decoding and encoding run on the hardware engines asynchronously; the main loop only starts jobs and takes
  completions.
