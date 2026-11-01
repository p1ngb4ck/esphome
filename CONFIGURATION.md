# Configuration reference

The first steps are in [README.md](README.md). Video features need the ESP32-P4 (hardware JPEG / H.264, PPA, MIPI
DSI / CSI) and PSRAM.

- [`mipi_csi`](#mipi_csi) - MIPI-CSI cameras
- [`camera` encoder, `rtsp_server`](#camera-encoder-and-rtsp_server)
- [LVGL `video_output`](#lvgl-video_output)
- [`avi`](#avi) - AVI media player
- [`screen_capture`](#screen_capture)
- [`stream_receiver`](#stream_receiver)
- [`apps`](#apps)
- [Media player options](#media-player-options) - pictures, controls, queue, extended API services
- [`online_image` from a path](#online_image-from-a-path)

## `mipi_csi`

A MIPI-CSI camera as a `camera` entity (`name`, ...). Needs `i2c:` (SCCB) and `psram:`.

| Option | Default | Description |
|---|---|---|
| `sensor` | required | Built in: `GC2053`, `GC2145`, `GC2607`, `GC4053`, `OV02C10`, `OV2710`, `OV5640`, `OV5645`, `OV5647`, `OV9281`, `SC035HGS`, `SC121AT`, `SC132GS`, `SC1346`, `SC202CS`, `SC2331`, `SC2336`, `SC2337P`. Another name works when its driver comes from another IDF component. |
| `resolution` | the sensor's default | `WIDTHxHEIGHT`, one the sensor offers. |
| `pixel_format` | `RGB565` | `RGB565`, `RGB888`, `YUV422`, `GRAYSCALE`. |
| `framerate` | `10 fps` | 1-60. |
| `jpeg_quality` | `40` | 10-100, the hardware JPEG encoder. |
| `encoder` | `jpeg` | `type: jpeg` or `type: h264` (`bitrate` 2000000, `gop` 30, `fps` 30, `qp_min` 25, `qp_max` 40). |
| `frame_buffer_count` | `4` | 2-4. |
| `sccb_frequency` | required | I2C frequency of the sensor's control bus, 10 kHz - 1 MHz. |
| `i2c_id` | the only bus | The sensor's I2C bus. |
| `reset_pin` / `power_pin` | - | Sensor reset / power-down pins. |
| `external_clock` | - | A clock the P4 generates for the sensor: `pin` (required), `frequency` (`24MHz`, 6-40 MHz). |
| `init_ldo` | `true` | Power the MIPI PHY from the P4's LDO. |
| `horizontal_flip` / `vertical_flip` | the sensor's | Mirror the image. |
| `lvgl_id` | - | Show the frames through LVGL's `video_output`. |
| `on_stream_start` / `on_stream_stop` | - | A client started / stopped streaming. |

```yaml
mipi_csi:
  name: Camera
  sensor: OV02C10
  resolution: 1280x720
  framerate: 30 fps
  sccb_frequency: 400kHz
  external_clock:
    pin: GPIO11
    frequency: 24MHz
  encoder:
    type: h264
    bitrate: 3000000
```

## Camera encoder and `rtsp_server`

Every camera with an `encoder` option (`mipi_csi`, `usb_uvc`) streams JPEG or H.264. `rtsp_server` takes
`format: MJPEG` (default) or `H264` (RFC 6184); H.264 needs a camera with `encoder: type: h264`. API clients get
camera images only as JPEG.

## LVGL `video_output`

The display's video output: hardware JPEG decoding straight into the display's frame buffers, shown in LVGL
canvases. Needs a MIPI DSI display with scanout (`frame_buffers`, esphome-mipi-display).

| Option | Default | Description |
|---|---|---|
| `canvases` | required | 1-2 LVGL canvases the video appears in (they follow the frame size). |
| `min_fps` | `25` | Lowest frame rate the output plans for. |

```yaml
lvgl:
  video_output:
    canvases: [video_canvas]
  pages:
    - id: video_page
      widgets:
        - canvas:
            id: video_canvas
            width: 100%
            height: 100%
```

## `avi`

A media player for AVI files and streams: MJPEG video through the video output, PCM / MP3 / FLAC sound to a
speaker.

| Option | Default | Description |
|---|---|---|
| `display_id` | required | The display (video output). |
| `speaker` | - | The sound output. |
| `buffer_size` | `2MB` | Read-ahead ring buffer in PSRAM, 256 KB - 16 MB. |
| `frame_size` | `512kB` | Largest video frame, 64 KB - 2 MB. |
| `audio_codecs` | `[pcm]` | Codecs built in: `pcm`, `mp3`, `flac`. |
| `audio_latency` | `0ms` | Delay of the sound path to compensate (a time, or a lambda returning milliseconds, e.g. a Bluetooth speaker's delay). |

Plus the media player options below (`pictures`, `controls`, `queue`, ...). Files are read through the storage
worker (`file://` paths and plain paths); HTTP streams too.

```yaml
media_player:
  - platform: avi
    id: video_player
    name: Video
    display_id: main_display
    speaker: main_speaker
    audio_codecs: [pcm, mp3]
    controls:
      lvgl_id: lvgl_main
```

## `screen_capture`

Screenshots (JPEG) and screencasts (AVI, MJPEG + sound) of what LVGL shows.

| Option | Default | Description |
|---|---|---|
| `screenshot_directory` | required | Storage folder for screenshots. |
| `screencast_directory` | - | Storage folder for screencasts. |
| `jpeg_quality` | `80` | 1-100. |
| `fps` | `25` | Screencast frame rate, 1-30. |
| `target` | `local` | Screencasts to `local` files, the `relay` (Home Assistant), or `both`. |
| `microphone` | - | A microphone source recorded with the screencast. |
| `sound` | `true` | Record what the device plays. |
| `relay` | `false` | Allow captures to the relay link (esphome-relay-link). |
| `on_screenshot` / `on_screencast_started` / `on_screencast_stopped` | - | Events. |

```yaml
screen_capture:
  screenshot_directory: /sdcard/screenshots
  screencast_directory: /sdcard/screencasts

button:
  - platform: template
    name: Screenshot
    on_press:
      - screen_capture.screenshot:
```

## `stream_receiver`

A media player that shows a stream sent by Home Assistant (a remote desktop or browser rendered there): screen
rectangles into an LVGL canvas, sound to a speaker, touches back to the sender, AVI streams through an `avi`
player.

| Option | Default | Description |
|---|---|---|
| `port` | required | TCP port the sender connects to. |
| `canvas_id` | - | The LVGL canvas the picture goes to (with the video output's page mode). |
| `speaker` | - | Sound output. |
| `touchscreen_id` | - | Touches sent back to the sender. |
| `avi_player` | - | An `avi` media player for AVI streams. |
| `max_frame_size` | `128kB` | Largest update, 16 KB - 1 MB. |
| `channels` | - | A channel list from the sender; `widget_id`: an LVGL dropdown that shows it. |

Actions: `media_player.channels.refresh`, `media_player.channels.play` (`index`), `media_player.channels.stop`,
`media_player.stream.output` (`fullscreen`: true / false).

## `apps`

Apps that share the screen: starting one stops the ones it is interlocked with.

| Option | Default | Description |
|---|---|---|
| `id` | required | The app. |
| `interlock` | `[]` | Apps stopped when this one starts. |
| `on_start` / `on_stop` | - | What starting / stopping does (open the page, start the player, ...). |

```yaml
apps:
  - id: video_app
    interlock: [camera_app, games_app]
    on_start:
      - lvgl.page.show: video_page
  - id: camera_app
    interlock: [video_app, games_app]
  - id: games_app
    interlock: [video_app, camera_app]
```

Actions `apps.start` / `apps.stop` (the app ID), condition `apps.is_running`.

## Media player options

Every media player (`speaker`, `avi`, `usb_audio`, ...) can take these blocks.

### `pictures`

Pictures and slideshows in an `online_image`.

| Option | Default | Description |
|---|---|---|
| `image_id` | required | The `online_image` that shows the picture. |
| `directories` | `[]` | Storage folders offered as picture sources. |
| `slideshow_interval` | `5s` | Time per picture. |
| `default_image_index` | `0` | Picture shown first, or `off`. |
| `thumbnails` | `[]` | Up to 50 `online_image`s for thumbnails. |
| `on_picture` | - | A picture is shown. |

Actions: `media_player.pictures.show`, `open_directory`, `set_interval` (`interval`), `thumbnail_show`.

### `controls`

An on-screen control bar (LVGL) over the video.

| Option | Default | Description |
|---|---|---|
| `lvgl_id` | the only one | The LVGL instance. |
| `buttons` | `[previous, play_pause, stop, next]` | From `previous`, `play_pause`, `stop`, `next`, `volume_down`, `volume_up`, `exit`. |
| `hide_after` | `5s` | Hide the bar after this long without input. |
| `title` | `true` | Show the title. |
| `bar_style_id`, `button_style_id`, `accent_button_style_id`, `focus_style_id` | - | LVGL styles for theming. |
| `on_exit` | - | The `exit` button. |

Action: `media_player.controls.set_title`.

### `queue`

A play queue.

| Option | Default | Description |
|---|---|---|
| `max_entries` | `500` | 1-5000. |
| `relay` | `false` | Items from Home Assistant through the relay link. |

Actions: `media_player.queue.add` (`path`, `title`), `media_player.queue.play`, `media_player.queue.set_shuffle`,
`media_player.queue.set_repeat` (`repeat`: `off`, `one`, `all`).

### `extended_api_services`

API services for Home Assistant (play with metadata, commands, volume, cover art) and a status text sensor.

| Option | Default | Description |
|---|---|---|
| `status` | `{name: Media status}` | Text sensor with the player's status. |
| `cover_canvases` | - | 1-3 LVGL canvases that show the cover art (needs `lvgl_id`). |
| `lvgl_id` | - | The LVGL instance. |

```yaml
media_player:
  - platform: speaker
    id: music
    extended_api_services: true
    queue:
      max_entries: 1000
```

## `online_image` from a path

`online_image` takes `url` or `path`: a file on a storage, read through the storage interface. The format follows
the file extension unless `format` is given.

```yaml
online_image:
  - id: picture
    path: /sdcard/pictures/start.jpg
    format: JPEG
    type: RGB565
```
