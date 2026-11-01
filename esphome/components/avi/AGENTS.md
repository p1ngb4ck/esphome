# avi: guide for AI agents

AVI files and streams: demuxer / muxer (`avi_parser.*`), a read-ahead reader on the storage worker
(`buffered_file_reader.*`), `cast_avi.*` (the OpenDML AVI writer of screencasts), and the `media_player` platform
(`media_player/avi_media_player.*`).

- The reader fills a PSRAM ring (`buffer_size`) through worker streams; the player never reads files on the main
  loop.
- Video: MJPEG frames to the display's video output (hardware JPEG). Sound: PCM / MP3 / FLAC through the audio
  decoder to the speaker; `audio_latency` shifts the sound clock to match the output's delay.
- The sound and the picture each run on their own clock; the picture is never paced after the sound and frames are
  never dropped.
- `last_error`, `current_file` and the media player state are the player's interface for UIs.

Video rules (all video components):
- Never touch LVGL while LVGL is paused or the video output writes the frame buffers directly.
- The device never rescales or resizes at runtime beyond what the hardware (PPA, JPEG decoder) does in one pass;
  frames are not dropped to keep up - fix the pipeline instead.
- Frame and stream buffers live in PSRAM, are allocated when playback / capture starts and freed when it stops.
- Decoding and encoding run on the hardware engines asynchronously; the main loop only starts jobs and takes
  completions.
