# esphome-esp-video

Video for ESPHome on the ESP32-P4: MIPI-CSI cameras with the hardware JPEG and H.264 encoders, RTSP streaming, the
display's **video output** (hardware JPEG decoding straight into the display's frame buffer, beside or instead of
LVGL), an AVI player with sound, screenshots and screencasts of the UI, a stream receiver for a desktop / browser
streamed from Home Assistant, pictures, slideshows, play queues and on-screen controls for every media player, and
an app structure that keeps apps from fighting over the screen.

## Getting started

A MIPI-CSI camera shown in LVGL and streamed over RTSP:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-esp-video@main
    components: [esp32, camera, camera_encoder, mipi_csi, rtsp_server, display, lvgl, mipi_dsi]

mipi_csi:
  name: Camera
  sensor: OV5647
  resolution: 1280x720
  sccb_frequency: 400kHz
  encoder:
    type: h264
  lvgl_id: lvgl_main

rtsp_server:
  format: H264
```

An AVI player that plays MJPEG videos through the video output:

```yaml
lvgl:
  id: lvgl_main
  video_output:
    canvases: [video_canvas]

media_player:
  - platform: avi
    id: video_player
    display_id: main_display
    speaker: main_speaker
    audio_codecs: [pcm, mp3]
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option - `mipi_csi` (sensors, formats, encoder, flip, clocks),
`camera` encoder, `rtsp_server`, LVGL `video_output`, `avi`, `screen_capture`, `stream_receiver`, `apps`, the media
player's `pictures` / `controls` / `queue` / `extended_api_services`, `online_image` from a storage path.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The video output builds on
the display topic's scanout ([esphome-mipi-display](https://github.com/p1ngb4ck/esphome-mipi-display)
`pr/mipi-dsi-scanout`) and LVGL's remote input ([esphome-hid](https://github.com/p1ngb4ck/esphome-hid)
`pr/lvgl-remote-input`), so `main` contains those.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/esp32-require-hw-jpeg` | esp32 `require_hw_jpeg()`: the JPEG codec for the chip (P4 hardware JPEG, S2 / S3 esp_new_jpeg, others JPEGDEC) | - | all |
| `pr/runtime-image-jpeg-decoder` | runtime_image: the JPEG decoder on the selected backend | `pr/esp32-require-hw-jpeg` | all |
| `pr/online-image-path-source` | online_image: `url` or `path`, a local path through the storage interface | `pr/storage` | all |
| `pr/camera-encoder-h264` | camera, camera_encoder: H.264 streams on the P4's hardware encoder | - | P4 |
| `pr/api-camera-jpeg-only` | api: camera images to clients only when JPEG | `pr/camera-encoder-h264` | all |
| `pr/rtsp-server-h264` | rtsp_server: H.264 streams (RFC 6184), any camera | `pr/camera-encoder-h264` | P4 (H.264) |
| `pr/mipi-csi` | mipi_csi: the MIPI-CSI camera of esphome/esphome#19480 | - | P4 |
| `pr/mipi-csi-on-demand` ... `pr/mipi-csi-hw-jpeg` | mipi_csi: on demand with the caller's memory, esp_ipa, image controls and flips, direct frames, exposure settling, H.264, the OV02C10, `USE_MIPI_CSI`, the hardware JPEG codec; each branch contains the one before it | `pr/mipi-csi`, `pr/esp32-idf-component-override-path` | P4 |
| `pr/esp32-require-h264-encoder` | esp32 `require_h264_encoder()` | `pr/esp32-require-hw-jpeg`, `pr/camera-encoder-h264` | P4 |
| `pr/camera-encoder-option` | camera, mipi_csi: the encoder a camera option (`encoder: jpeg \| h264`) | `pr/esp32-require-h264-encoder`, `pr/mipi-csi-hw-jpeg` | P4 |
| `pr/avi` | avi: AVI demuxer, muxer, read-ahead file reader on the storage worker | `pr/storage` | all |
| `pr/display-video-output` | display, lvgl: `display::VideoOutput` - JPEG frames straight into the scanout, LVGL `video_output:` (1-2 canvases), page mode, the camera scanout layer, frame capture | `pr/mipi-dsi-scanout`, `pr/lvgl-color-depth-32`, `pr/lvgl-remote-input`, `pr/esp32-require-hw-jpeg` | P4 |
| `pr/display-video-output-page-draw` | display: page draw calls (`page_rect`, `page_fill`, `page_pixels`, `page_copy`, `page_read`, `page_show`, `page_bpp`, `canvas_point`) | `pr/display-video-output` | P4 |
| `pr/display-video-output-rows` | display: `open_rows`, `rows_begin` / `rows_end` for a writer of its own, `rotation()`, `display()` | `pr/display-video-output-page-draw` | P4 |
| `pr/avi-media-player` | avi: media_player platform (MJPEG through the video output, PCM / MP3 / FLAC sound, playlist, network streams) | `pr/avi`, `pr/display-video-output` | P4 |
| `pr/screen-capture` | screen_capture: screenshots and screencasts of what LVGL shows | `pr/avi`, `pr/display-video-output` | P4 |
| `pr/apps` | apps: app structure with interlock (`apps.start` / `stop` / `is_running`) | - | all |
| `pr/mipi-csi-video-output` | mipi_csi: runtime resolutions, frames through the video output | `pr/camera-encoder-option`, `pr/display-video-output` | P4 |
| `pr/stream-receiver` | stream_receiver: media_player platform for a stream from Home Assistant (screen rectangles into a page canvas, sound, touches, AVI through the avi player) | `pr/avi-media-player` | P4 |
| `pr/media-player-options` | media_player: `pictures`, `controls`, `queue`, `extended_api_services` for every media player | `pr/display-video-output`, `pr/relay-link` | all (LVGL parts: P4) |

`pr/storage` is in [esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface),
`pr/esp32-idf-component-override-path` in [esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform),
`pr/relay-link` in [esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link). usb_uvc's video output part
is `pr/usb-uvc-video-output` in [esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

`mipi_csi` is built on Horstexplorer's MIPI-CSI camera component
([esphome/esphome#19480](https://github.com/esphome/esphome/pull/19480)) - thank you for opening the door to
cameras on the ESP32-P4. Thank you, Mallow ([youkorr](https://github.com/youkorr)): the camera and the dashboard
build on your ESP32-P4 work, and the extra OV02C10 resolutions come from your esp_video. The OV02C10 driver and
esp_ipa are Espressif's (Apache-2.0). Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
