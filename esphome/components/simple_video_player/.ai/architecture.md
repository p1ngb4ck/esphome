# svp architecture — as the code is today (2026-10-15, commit 6a5877bba8)

Facts with file:line, read from the code. File keys: `S` simple_video_player.cpp, `H` .h,
`R` stream_receiver.cpp, `P` pictures.cpp, `C` audio_cover.cpp, `CL` channel_list.cpp,
`CT` controls.cpp, `MS` media_services.cpp, `SP` spice_client.cpp, `PP` psram_pool.cpp,
`U` select/spice_usb.cpp, `UI` select/usbip.cpp, `BFR` buffered_file_reader.*.
Line numbers drift: search the symbol when a line doesn't match.

## 1. Files

| File | Job |
|---|---|
| simple_video_player.cpp/.h | Component, main loop, sessions, playback task, reader, audio task, pool plan + media buffers, DSI/canvas output |
| psram_pool.cpp/.h | `PsramPool`: one block (`jpeg_alloc_decoder_mem`, zeroed), first-fit, 128-aligned pieces, merge on free, own mutex (PP:7-66). `pieces_` vector on the internal heap (reserves 32) |
| buffered_file_reader.* | Read-ahead ring over the storage interface; push mode for network AVI |
| avi_parser.* | AVI/MJPEG parsing, index |
| stream_receiver.cpp | TCP stream port: AVI from relay, udisp stills/PCM/input, draw task, 'V'/'A'/... reports |
| pictures.cpp | Picture viewer, thumbnails, picture task (HW JPEG, PNG/BMP sw) |
| audio_cover.cpp | Cover art of the audio queue's HA track in up to 3 canvases |
| channel_list.cpp | Channel task: all HTTP to svp_relay (channels, play/stop, browse, image, options, cover, desktop) |
| media_services.cpp | Volume, status JSON, HA services (`svp_*`), media player link, audio router callback |
| controls.cpp | On-screen controls: video bar (pre-rendered, composited in DSI direct), picture bar and audio bar (LVGL) |
| spice_client.cpp | SPICE and VNC (via relay) remote desktop, cursor, desktop panel takeover |
| select/spice_usb.*, usbip.cpp | USB redirection slots (usbredir, USB/IP) |
| number/, text_sensor/ | Volume number, status text sensor |
| `__init__.py` | Config schema and codegen (defines `SVP_*`) |

## 2. Tasks

All via `create_psram_task` (H:79-83): stack in PSRAM (size in bytes), TCB internal.
Main loop: `loopTask`, prio 1, core 1 (`CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1`, `App.set_loop_interval(1)`).

| Task | Entry | Core | Prio | Stack | Job / blocks on |
|---|---|---|---|---|---|
| video_player | `playback_task_entry_` S:856 | 1 | 1 | 8192 | playback session; notify, `playback_wait_` (Waiter), `output_lock_` for the whole session (S:1186), `media_lock_`, `wait_alloc_answer_` (<=2 s), JPEG `codec_mutex` |
| svp_reader | `reader_task_entry_` S:871 | 0 | 2 | 4096 | video file / audio file read; `reader_wait_`, BFR reads in 20 ms slices |
| svp_audio | `audio_task_entry_` S:2741 (only with a speaker) | 0 | 1 | 4096 | audio decode/feed; `audio_wait_`, ring reads 20 ms, `speaker->play` 20 ms |
| svp_pictures | `picture_task_entry_` P:917 | 1 | 1 | 8192 | picture decode and cover decode (cover job first) |
| svp_channels | `channel_task_entry_` CL:251 | 0 | 1 | 8192 | HTTP to relay through the `http_request` component (its timeout), queue `channel_q_` (8) |
| svp_spice | `spice_task_entry_` SP:2585 | any | 4 | 8192 | SPICE/VNC session, select loops, USB service |
| svp_stream_draw | `stream_draw_task_entry_` R:1010 | 1 | 2 | 4096 | decode/draw udisp stills, desktop cursor |
| svp_stream_net | `stream_net_task_entry_` R:271 | any | 4 | 4096 | accept/serve one sender at a time, push AVI into ring |
| svp_pace (esp_timer) | `pace_timer_cb_` H:2089 | – | – | – | wakes `playback_wait_` at the frame instant |

## 3. Locks (and the order they nest in)

| Lock | Taken by |
|---|---|
| `media_lock_` (S:62) | playback/draw tasks blocking; main loop try-0 only |
| `session_lock_` (S:63) | main loop try-0 (`run_pending_start_`); net task 10 s |
| `output_lock_` (R:104) | playback task for the whole session (S:1186-1414); draw task try-0; main loop try-0; `release_desktop_` blocking |
| `desktop_lock_` (R:194) | `desktop_buffers_take_`, `release_desktop_` |
| `spice_lock_`, `spice_cursor_lock_` (R:192-193) | main loop / spice / net / draw tasks |
| `channel_lock_` (CL:34) | main loop and channel task (main loop holds it across `set_options`, CL:197-209) |
| `PsramPool::lock_` | leaf, any task |
| `BFR::stream_lock_`, `usbip_lock_` | leaf |
| `SpiceUsbSlot::dev_lock_` (U:131) | main loop portMAX; spice task, partly across socket sends |

Nesting in use: `media_lock_` -> pool; `media_lock_` -> `wait_alloc_answer_` (blocks up to 2 s
holding it); `media_lock_` -> `desktop_lock_` -> `output_lock_` -> `spice_cursor_lock_`
(`media_alloc_` -> `release_desktop_`); main loop `output_lock_` -> `media_lock_` (both try-0, so no
deadlock); draw task `output_lock_` -> `spice_cursor_lock_`; `output_lock_` -> `codec_mutex`.

## 4. Cross-task handshakes (atomic flags)

| Flag | Set by | Serviced by (main loop unless noted) | Wait |
|---|---|---|---|
| `pool_reclaim_req_` | `media_alloc_` S:2155, `acquire_session_buffers_` S:2177, `desktop_buffers_take_` SP:1533 | `pictures_loop_` P:151-173: in-flight read/decode or cover fetch finishes, then `release_cover_buffers_` + `release_picture_buffers_`, `answer_alloc_` | `wait_alloc_answer_` <=2 s |
| `output_release_req_` | `output_alloc_` S:2214-2217 | `handle_output_release_` S:2292 | <=2 s |
| `media_release_req_` | `desktop_buffers_take_` SP:1537 | loop S:257-261 -> `release_media_` | <=2 s |
| `alloc_waiter_` | `wait_alloc_answer_` H:2110 (single slot) | `answer_alloc_` H:2119 | – |
| `start_req_` 0/1/2 | playback S:1190 | loop S:291-306: prerender controls, pause LVGL, on_started, ->2 | <=2 s |
| `advance_req_` | playback EOF S:1315 | loop S:279-287 | – |
| `audio_advance_req_` | audio task S:3225, media player callback CL:101 | loop S:249-254 | – |
| `cover_job_`/`cover_decoded_`, `cover_fetched_` | `cover_loop_` / channel task | picture task / `cover_loop_` (polled) | – |
| `pic_decoded_`, `pic_read_.done` | picture task / channel task / storage callbacks | `pictures_loop_` (polled) | – |
| `stream_drop_sender_` | spice SP:1486 | net task R:390 | spice polls 20 ms, <=3 s |

## 5. Main loop order (`loop()` S:225-443)

1 file readers `service()` · 2 `handle_output_release_` · 3 finished/error/state callbacks ·
4 audio callbacks + `audio_advance_` · 5 `media_release_req_`, SPICE sound claim/callbacks ·
6 `run_pending_start_` · 7 `channel_loop_`/`ha_loop_` · 8 `pictures_loop_` · 9 `cover_loop_` ·
10 `advance_req_` · 11 `controls_loop_` · 12 `start_req_==1` (prerender, pause LVGL) ·
13 DSI pause/unpause, paused UI, stream audio timeout, `stream_output_ok_` ·
14 stream canvas dirty invalidate · 15 canvas frame present (`frame_ready_`).

## 6. Sessions and modes

`state_` STOPPED/PLAYING/PAUSED/ERROR (H:94) is the one session state. `get_state()` (S:688)
reports the media player's state instead while `state_` is STOPPED and `audio_remote_ != NONE`.
`session_busy_()` = playback, reader or audio task busy (S:505). Starts are serialised by
`session_lock_` and `run_pending_start_` (S:513).

| Kind | Entry | Runs on | Ends / excludes |
|---|---|---|---|
| Local video file | `play`/`play_index_`/`next`/`previous` -> `run_pending_start_` -> `start_playback_` S:651 | playback + reader + audio tasks | EOF -> playlist advance; ends audio file session; holds pictures/cover; refuses network AVI while running |
| Network AVI (relay) | net task, 'RIFF' R:442-489, `net_session_ = true` | same as file, ring fed by `push()` | no seek, no loop, no controls; EOF -> 'E' to relay; refused while PLAYING/PAUSED |
| udisp stills/PCM (dashboard, desktop picture) | sender connects R:298-351 | draw task (stills), net task (PCM) | not drawn while a file plays; 30 s silence ends it; desktop takeover drops it |
| Audio queue, local file | `audio_start_` S:2918 -> `start_audio_session_` S:3094 | reader (`audio_file_feed_`) + audio task (`audio_file_loop_`) -> `file_speaker_` | pictures keep running; video/AVI end it |
| Audio queue, HA track | `audio_start_` HA branch S:2936: `audio_remote_ = ASKED`, POST /play | relay -> `svp_play_audio` -> media player | `audio_remote_player_` CL:89: ASKED+PLAYING -> PLAYING; PLAYING+IDLE (>=2 s) -> next. Any `start_playback_` sets NONE (S:669) |
| Pictures | `show_image`, next/prev, slideshow, `ha_show_pictures` | main loop + picture task; HA reads on channel task (/image) | held while a video plays (`pictures_held_` P:126, by `state_` only) |
| Cover art | `svp_cover` service -> `cover_wanted_` | `cover_loop_` C:121 (main loop, not while LVGL paused) -> channel task (/cover) -> picture task | buffers given back on pool reclaim (C:87) |
| Remote desktop | `spice_connect(_saved)` -> spice task | spice task; picture via relay udisp | `desktop_take_panel_` SP:1481 ends media, takes buffers; refuses media starts while `spice_on_` |
| Controls | video bar: pre-rendered at `start_req_==1`, composited by playback (DSI direct only, not for network AVI); picture/audio bars: LVGL | main loop / playback | – |

## 7. Output paths

- **DSI direct**: only when the video equals the panel's native size (S:997-1016). Decoded by HW JPEG
  into `decode_target_` (display depth), composited controls, then `dsi_->draw_pixels_at()` copies it
  into the panel framebuffer every frame (S:1395-1403). LVGL paused by `loop()` only (S:297-331).
  No PPA in svp.
- **Canvas**: RGB888 pair `output_buffers_[2]` at canvas size; decode into the back buffer, loop swaps
  the canvas draw buffer (S:419-441, S:1612-1625).
- **udisp stills**: draw task decodes into the direct target or the canvas pair (R:1014-1153).
- Rotation: direct video must be native; relay is told size + quarter turns ('V', R:773-790);
  LVGL rotates the canvas; controls sprites rotated in software.

## 8. Audio paths

| Path | Producer | Consumer -> output |
|---|---|---|
| Video sound | reader `process_audio_frame_` S:2604: compressed -> input ring, PCM -> decoded ring | audio task: decoder sink `speaker_` (S:2577), PCM drained to `speaker_` (S:2832). Player never starts/stops `speaker_` (f0b518d) |
| Stream PCM | net task / spice playback channel | `stream_play_audio_` R:1166: 10 ms blocks, non-blocking play; stream info only set while the speaker isn't running. Dropped unless `state_` STOPPED |
| Audio files | reader `audio_file_feed_` (128 KB ring) | audio task `audio_file_loop_` -> `file_speaker_` (resampler); player still calls `stop`/`finish`/pause on it |
| HA audio | `svp_play_audio` MS:132 | speaker media player (own pipeline) |
| Router | `add_on_output_callback` S:50 -> `on_audio_output_changed_` MS:170 | new wanted rate/channels, resend 'A/L/N/F' to the sender |
| Router format | players only `set_audio_stream_info` | `audio_router` starts its output with the first sound, and starts it again (stop, wait until stopped, start) when a player's stream format differs from the one it was started for (audio_router.cpp `play()`); the player never starts/stops |

A/V sync: `audio_latency` (BT delay; device YAML: only while the media output is Bluetooth) clamped
to `max_audio_latency_ms_` = `MAX_AUDIO_LEAD_MS` 1600 ms (S:680); the sound buffers are sized for it; sound released when the cursor leads by `audio_lead_frames_`; picture clock anchored that
much later (S:1199-1211, S:1332-1336).

## 9. PSRAM pool and buffers (today)

Pool: `psram_pool_size` (default 12 MiB, 6-28; device 13 MB = 13,000,000). Plan in `plan_pool_()`
S:2080-2148: everything listed as "fixed" is reserved; the **read-ahead ring gets all the rest**
(whole 256 KiB chunks); the audio rings are sized for `MAX_AUDIO_LEAD_MS` (1600 ms).

| Buffer | Size | Taken / returned | Content | In plan |
|---|---|---|---|---|
| `cache_buffer_` | 64 KiB | setup, never | raw reads, index/audio scratch | yes |
| audio input ring | rate·ch·2·(lat+250 ms), >=32 KiB | setup, never | compressed audio | yes |
| audio decoded ring | rate·ch·2·max(lat+250, 500 ms) | setup, never | PCM (only PCM tracks) | yes |
| `audio_temp_buffer_` | max(BPS/10, 8 KiB) | setup, never | PCM drain / file read | yes |
| audio-file ring | 128 KiB | setup, never | compressed audio file | yes |
| `stream_recv_buf_` | 32 KiB | setup, never | TCP bytes | yes |
| `stream_frames_[4]` | 4 × frame_bytes_/4 | setup, never | compressed JPEG stills | yes |
| `stream_audio_block_` | 10 ms PCM | setup, never | PCM | yes |
| stream input queue | 384 B | setup, never | input messages | no |
| SPICE state/saved/file | ~8 KB | setup, never | config | yes |
| `thumb_buffer_` | stride·th·count (~216 KiB default) | setup, never | decoded thumbnails | yes |
| `frame_slots_[1]` | frame_bytes_ = clamp(pool/24, 256 KiB, 1 MiB) | first session, kept | compressed frame | yes |
| `index_buf_` | clamp(pool/32, 128 KiB, 1 MiB) | first session, kept | AVI index (also allocated for streams) | yes |
| `ring_buf_` | rest of pool (~7-8 MB on the device) | first session, kept | compressed AVI/MJPEG | = rest |
| `decode_target_` + `ctl_mem_` | panel w·h·bpp (2.05 MB) + 3 × bar snapshot + 4 KiB | first direct output, kept | decoded frame, controls | yes (max of direct/canvas) |
| `output_buffers_[2]` | 2 × A16(cw)·A16(ch)·3 | first canvas output, kept | decoded RGB888 | yes (max of direct/canvas) |
| `pic_pages_[2]` | 2 × stride565(max(disp, canvas)) · h | first picture, reclaim | the shown picture and the next one (no decode ahead; neighbours only read undecoded) | **no** |
| `pic_decode_buf_` | the decode's size in 1 MiB steps | only while a picture larger than its target (or a thumbnail) is decoded | full native decode | **no** |
| `pic_files_[3]` | 1 MiB each, grows | first picture, reclaim | compressed files | **no** |
| `cover_views_[<=3]` + `cover_file_` | A16 w·h·2 each (device 25 K + 320 K + 1.28 MB) + 512 KiB | cover wanted, reclaim | decoded cover / JPEG | **no** |
| desktop cursors | ~300 KiB | session start, `release_desktop_` | cursor shapes | no |
| USB transfers per slot | ~550 KiB at link_open + ISO | link open, `release_buffers` | USB data | no |

Reclaim/handover (as coded):
- `media_alloc_` (S:2150, tasks only): pool -> pictures/cover reclaim (main loop) -> `release_desktop_`.
- `acquire_session_buffers_` (S:2170): requests the pictures/cover reclaim first when a media buffer is
  missing (one free stretch for the ring), then slot -> index -> ring.
- `output_alloc_` (S:2207): pool -> main loop frees the other output (`handle_output_release_`) ->
  `media_alloc_`.
- `release_media_` (S:2344, main loop): frees ring/slot/index/outputs/controls when no video runs;
  then `release_desktop_`. Called by `picture_alloc_` P:494 and `cover_alloc_` C:59.
- `desktop_buffers_take_` (SP:1528): pictures/cover reclaim + `media_release_req_`, then cursors.

Outside the pool (svp): `spice_scratch_` 20 KiB internal; per USB slot 4 KiB internal `stage_` +
bookkeeping; task stacks 48 KiB PSRAM; PsramVector/PsramString lists (playlist, images, queue,
HA items); `channel_http_` response buffer PSRAM 16-256 KiB per request; AudioDecoder internals;
PNG/BMP decode (RAMAllocator); LVGL objects; mbedtls per SPICE connect.

## 10. Home Assistant / relay interface (summary; details in protocol.md)

- HA services (only with `media_player_id`, `api: custom_services: true`): `svp_play_audio`,
  `svp_audio_command`, `svp_set_volume`, `svp_cover`, `svp_stream_statistics`.
- Status text sensor JSON (MS:41-71): `svp`, `port`, `state` (media player's), `volume`, `muted`,
  `playlist`, `pause`, `formats`.
- Panel -> relay HTTP on the channel task: `/channels`, `/play`, `/stop`, `/browse`, `/image`,
  `/cover`, `/options`, `/desktop`.
- Stream port: AVI ('RIFF') or udisp (16-byte header); panel reports 'C','V','A','L','N','F','S'.
