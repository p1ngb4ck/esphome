# esphome-esp-video

ESPHome `dev` plus the video-related changes of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`:
codec requirements, image, camera, the display's video output, containers, players, screen capture, apps. One branch
per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from
`testing_dev` (the lines of esp32, runtime_image, online_image, camera, camera_encoder, mipi_csi, rtsp_server, api
assigned by `git blame`); `main` equals `testing_dev` for runtime_image, online_image, camera, camera_encoder, mipi_csi,
rtsp_server, display, avi, screen_capture, apps and stream_receiver, and for lvgl except the color picker, the keyboard
text and the i18n label changes (other topics). The video output builds on the display topic's scanout
([esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display) `pr/mipi-dsi-scanout`) and lvgl's remote
input ([esphome-hid](https://github.com/p1ngb4ck/esphome-hid) `pr/lvgl-remote-input`), so `main` contains those.

The structure this repo grows into (codecs required from esp32, image, camera, display output, containers, screen
capture, players): [the esp-video plan](https://github.com/p1ngb4ck/p4-panel-workspace) (private workspace,
`reports/2026-10-31-esp-video-structure-plan.md`).

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/esp32-require-hw-jpeg` | esp32 `require_hw_jpeg()`: the JPEG codec for the chip (P4 HW JPEG, S2 / S3 esp_new_jpeg, others JPEGDEC) | - | all |
| `pr/runtime-image-jpeg-decoder` | runtime_image: the JPEG decoder on the selected backend | `pr/esp32-require-hw-jpeg` | all |
| `pr/online-image-path-source` | online_image: url / path sources, a local path through the storage interface | `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | all |
| `pr/camera-encoder-h264` | camera, camera_encoder: H.264 streams on the P4's hardware encoder (contained in [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers) `main`, usb_uvc builds on it) | - | P4 |
| `pr/api-camera-jpeg-only` | api: camera images to clients only when JPEG | `pr/camera-encoder-h264` | all |
| `pr/rtsp-server-h264` | rtsp_server: H.264 streams (RFC 6184), any camera | `pr/camera-encoder-h264` | P4 (H.264) |
| `pr/mipi-csi` | mipi_csi: the MIPI-CSI camera of esphome/esphome#19480, unchanged | - | P4 |
| `pr/mipi-csi-on-demand` | mipi_csi: on demand with the caller's memory, preview target, its own HW JPEG encoder | `pr/mipi-csi`, `pr/esp32-idf-component-override-path` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) | P4 |
| `pr/mipi-csi-esp-ipa` | mipi_csi: esp_ipa override (library for P4 rev < 3), its log tags at WARN | `pr/mipi-csi-on-demand` | P4 |
| `pr/mipi-csi-controls` | mipi_csi: image controls, mirror / upside down | `pr/mipi-csi-esp-ipa` | P4 |
| `pr/mipi-csi-direct-frames` | mipi_csi: direct frames to a callback | `pr/mipi-csi-controls` | P4 |
| `pr/mipi-csi-log-name` | mipi_csi: dump_config through get_log_name() | `pr/mipi-csi-direct-frames` | P4 |
| `pr/mipi-csi-exposure-settle` | mipi_csi: no JPEG from a frame before the exposure settled | `pr/mipi-csi-log-name` | P4 |
| `pr/mipi-csi-h264` | mipi_csi: H.264 streams, frames for H.264 recording | `pr/mipi-csi-exposure-settle`, `pr/camera-encoder-h264` | P4 |
| `pr/mipi-csi-ov02c10` | mipi_csi: OV02C10 sensor | `pr/mipi-csi-h264` | P4 |
| `pr/mipi-csi-define` | mipi_csi: `USE_MIPI_CSI` | `pr/mipi-csi-ov02c10` | P4 |
| `pr/mipi-csi-hw-jpeg` | mipi_csi: the HW JPEG codec through `esp32.require_hw_jpeg()` | `pr/mipi-csi-define`, `pr/esp32-require-hw-jpeg` | P4 |
| `pr/esp32-require-h264-encoder` | esp32 `require_h264_encoder()` (esp_h264 on the P4); camera_encoder's h264 type requires it | `pr/esp32-require-hw-jpeg`, `pr/camera-encoder-h264` | P4 |
| `pr/camera-encoder-option` | camera, mipi_csi: the encoder a camera option (`encoder: jpeg \| h264`), camera_encoder back to upstream | `pr/esp32-require-h264-encoder`, `pr/mipi-csi-hw-jpeg` | P4 |
| `pr/avi` | avi: AVI demuxer, muxer, read-ahead file reader on the storage worker | `pr/storage` | all |
| `pr/display-video-output` | display, lvgl: the HW JPEG decoder's output straight into the scanout buffer, `display::VideoOutput` (lvgl `video_output:` 1-2 canvases following the frame size, page mode, capability report), the camera scanout layer, the frame capture, canvas `external_buffer` | `pr/mipi-dsi-scanout`, `pr/lvgl-color-depth-32` (esphome-mipi-display), `pr/lvgl-remote-input` (esphome-hid), `pr/esp32-require-hw-jpeg` | P4 (scanout) |
| `pr/display-video-output-page-draw` | display: video output page draw calls (`page_rect` a JPEG into a rectangle, `page_fill`, `page_pixels`, `page_copy`, `page_read`, `page_show`, `page_bpp`, `canvas_point`) | `pr/display-video-output` | P4 |
| `pr/display-video-output-rows` | display: `open_rows` (the panel's rows alone, no JPEG decoder), `rows_begin` / `rows_end` for a writer of its own (the emulator's PPA pass), `rotation()`, `display()` | `pr/display-video-output-page-draw` | P4 (scanout) |
| `pr/avi-media-player` | avi: media_player platform `avi` (MJPEG through the video output, PCM / MP3 / FLAC sound, playlist, network streams) | `pr/avi`, `pr/display-video-output` | P4 |
| `pr/screen-capture` | screen_capture: screenshots and screencasts of what LVGL shows | `pr/avi`, `pr/display-video-output` | P4 |
| `pr/apps` | apps: app structure with interlock (`apps.start` / `stop` / `is_running`) | - | all |
| `pr/mipi-csi-video-output` | mipi_csi: runtime resolutions, frames shown through the video output, formats the screen takes chosen at config time | `pr/camera-encoder-option`, `pr/display-video-output` | P4 |
| `pr/stream-receiver` | stream_receiver: media_player platform for svp_relay's stream (udisp rectangles into a page canvas, sound, touches, AVI through the avi player) | `pr/avi-media-player` | P4 |

usb_uvc's part (the encoder option, MJPEG frames through the video output) is `pr/usb-uvc-video-output` in
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers) (on `pr/usb-uvc`, `pr/camera-encoder-option`,
`pr/display-video-output`).

Each mipi_csi branch contains the one before it.

## Contributions

- [Horstexplorer](https://github.com/Horstexplorer): mipi_csi, esphome/esphome#19480.
- Mallow / [youkorr](https://github.com/youkorr): the camera and the dashboard (portall) build on his work; the
  OV02C10's 640x480 / 800x600 / 480x640 / 640x368 formats are from his esp_video.
- Espressif: the OV02C10 driver (Apache-2.0), esp_ipa of esp-video-components.

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-esp-video@main
    components: [esp32, runtime_image, online_image, camera, camera_encoder, mipi_csi, rtsp_server, display, lvgl,
                 mipi_dsi, avi, screen_capture, apps, stream_receiver]
```

`external_components` replaces components only; the ESPHome core of the build host stays as installed. With the other
topics together use the combined branch `github://p1ngb4ck/esphome@topics-combined`.
