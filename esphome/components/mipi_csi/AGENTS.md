# mipi_csi: guide for AI agents

MIPI-CSI cameras on the ESP32-P4 through esp_video / esp_cam_sensor.

- `mipi_csi.*`: `MipiCsiCamera` (a `camera`) - sensor init over SCCB, the CSI / ISP pipeline (esp_ipa, a library
  override for chip revisions below 3), runtime resolutions, image controls and flips, direct frames to a callback,
  preview frames to the video output, frames to the encoder (JPEG / H.264), exposure settling before the first JPEG.
- `jpeg_encoder.*`: the camera's own hardware JPEG encoder session.
- `sensors.py`: built-in sensors and their formats; another sensor's driver can come from an IDF component.
- `ov02c10/`: Espressif's OV02C10 driver (Apache-2.0) with extra formats.
- The camera runs on demand: streams start when a client, the video output or a recording asks, and stop (buffers
  freed) when the last one goes.

Video rules (all video components):
- Never touch LVGL while LVGL is paused or the video output writes the frame buffers directly.
- The device never rescales or resizes at runtime beyond what the hardware (PPA, JPEG decoder) does in one pass;
  frames are not dropped to keep up - fix the pipeline instead.
- Frame and stream buffers live in PSRAM, are allocated when playback / capture starts and freed when it stops.
- Decoding and encoding run on the hardware engines asynchronously; the main loop only starts jobs and takes
  completions.
