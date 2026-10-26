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
| psram_pool.cpp/.h | `PsramPool`: one block (`jpeg_alloc_decoder_mem`, zeroed), 128-aligned pieces, merge on free, own mutex. `alloc` first fit (buffers), `alloc_top` last fit from the end (svp's objects). Piece table (128) at the block's start |
| buffered_file_reader.* | Read-ahead ring over the storage interface; push mode for network AVI (a read waiting for data is woken by `push()` / `end_stream()`; it waits until data, the stream's end or the abort flag, not WAIT_CAP_MS: the network task's 30 s silence ends the stream) |
| avi_parser.* | AVI/MJPEG parsing, index |
| stream_receiver.cpp | TCP stream port: AVI from relay, udisp stills/PCM/input, draw task, 'V'/'A'/... reports |
| pictures.cpp | Picture viewer, thumbnails, picture task (HW JPEG, PNG/BMP sw) |
| audio_cover.cpp | Cover art of the audio queue's HA track in up to 3 canvases |
| channel_list.cpp | Channel task: all HTTP to svp_relay (channels, play/stop, browse, image, options, cover, desktop, keyboard) |
| keyboard_layouts.cpp | `keyboard_layouts:`: layouts (hid `.kbl`) per keyboard (`keyboards: [{id, name, layout}]`, its standard) and per device (`HIDInput::list_devices`: `usb:vid:pid[:serial]`, `bt:MAC`; keyboards only, by `InputDevice::kinds`: USB from its report descriptors, Bluetooth while connected from them too, else from the paired record's Class of Device / LE appearance), given from LVGL/HA (`keyboard_assign*`, action `svp_keyboard_layout`), kept in prefs (`KbdAssignTable`). Read from the directory (`directory:`, settable at runtime) via the storage worker first, else fetched from the relay's `/keyboard` and written there; downloads of several at once (`keyboard_download`); put on via `set_layout` / `set_device_layout`; Home Assistant: `device_select` (the standards and devices, labels/keys in the pool) and `layout_sensor` (the picked one's layout), svp_relay's "Keyboard layout" select sets it through `svp_keyboard_layout`; main loop |
| media_services.cpp | Volume, status JSON, HA services (`svp_*`), media player link, audio router callback |
| controls.cpp | On-screen controls: video bar (pre-rendered, composited in DSI direct), picture bar and audio bar (LVGL) |
| spice_client.cpp | SPICE and VNC (via relay) remote desktop, cursor, desktop panel takeover |
| select/spice_usb.*, usbip.cpp | USB redirection slots (usbredir, USB/IP) |
| screen_capture.cpp | `screen_capture:`: screenshots and screencasts of what LVGL shows (section 11) |
| cast_avi.* | `CastAvi`: the bytes of a screencast's AVI (OpenDML as ffmpeg's avienc: RIFF 'AVI ' + 'AVIX' up to 1 GB, ix00/ix01 per movi list, indx super indexes of 256 entries) |
| number/, text_sensor/ | Volume number, status text sensor |
| emulator.cpp/.h | `emulator:` (`SVP_EMULATOR`): `GameEmulator`, the game app (section 12) |
| emu_cores.cpp | The cores' C++ side (`EmuCore`), one class per system over its C port |
| emu_link.cpp | `emulator: link:` (`SVP_EMU_LINK`): the TCP link between two panels, netplay and cable (section 12) |
| emu/ | IDF component `emu` (`add_idf_component(override_path=...)`): the vendored cores (their authors' files, comments and licenses as they are), `ports/` (one C file per core: the narrow API svp calls), `shim/` (allocator hooks, `rg_system.h`), `linker.lf` (the component's `.bss` / `.sbss` / COMMON in PSRAM noinit, cleared at every game start), `Kconfig` (`CONFIG_SVP_EMU_<SYSTEM>`, set by codegen per `systems:` entry) |
| `__init__.py` | Config schema and codegen (defines `SVP_*`) |

## 2. Tasks

All via `create_psram_task` (H:79-83): stack in PSRAM (size in bytes), TCB internal.
Main loop: `loopTask`, prio 1, core 1 (`CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1`, `App.set_loop_interval(1)`).

| Task | Entry | Core | Prio | Stack | Job / blocks on |
|---|---|---|---|---|---|
| video_player | `playback_task_entry_` S:856 | 0 | 1 | 8192 | playback session; notify, `playback_wait_` (Waiter), `output_lock_` for the whole session (S:1186), `media_lock_`, `wait_alloc_answer_` (<=2 s), JPEG `codec_mutex` |
| svp_reader | `reader_task_entry_` S:871 | 0 | 2 | 4096 | video file / audio file read; `reader_wait_`, BFR reads in 20 ms slices |
| svp_audio | `audio_task_entry_` S:2741 (only with a speaker) | 0 | 1 | 4096 | audio decode/feed; `audio_wait_`, ring reads 20 ms, `speaker->play` 20 ms |
| svp_pictures | `picture_task_entry_` P:917 | 0 | 1 | 8192 | picture decode and cover decode (cover job first) |
| svp_channels | `channel_task_entry_` CL:251 | 0 | 1 | 8192 | HTTP to relay through the `http_request` component (its timeout; `/desktop`, `/image` 15 s), queue `channel_q_` (8) |
| svp_spice | `spice_task_entry_` SP:2585 | 0 | 4 | 8192 | SPICE/VNC session, select loops, USB service |
| svp_stream_draw | `stream_draw_task_entry_` R:1010 | 0 | 2 | 4096 | decode/draw udisp stills, desktop cursor |
| svp_stream_net | `stream_net_task_entry_` R:271 | 0 | 4 | 4096 | accept/serve one sender at a time, push AVI into ring |
| svp_pace (esp_timer) | `pace_timer_cb_` H:2089 | – | – | – | wakes `playback_wait_` at the frame instant |
| svp_capture | `capture_task_entry_` (screen_capture.cpp; made at the first capture, kept) | 0 | 3 | 4096 | copies the changed areas into the back picture after a swap, HW JPEG encodes, composes the screencast's batches; task notification bits |
| svp_cast_tx | `cast_tx_entry_` (made at the first screencast to svp_relay, kept) | 0 | 3 | 4096 | connects to svp_relay's cast port, sends the header and each batch (SO_SNDTIMEO 5 s) |
| svp_cast (esp_timer) | `capture_tick_cb_` | – | – | – | the screencast's tick (1/fps) to svp_capture |
| svp_game | `GameEmulator::task_entry_` (`esphome::StaticTask`, TCB and stack internal; made when a game's panel is ready, with a link before it to connect; deallocated after it ended) | 0 | 2 | 8192 | one emulated frame, PPA into the scanout buffer not shown and flip when the last flip is done (else that frame is not shown), the frame's sound into `speaker_` without waiting (what does not fit is dropped), then the frame's due time (`pace_`: a frame starts at the earliest `frame_us` after the one before, against `esp_timer_get_time()`; a frame that ends late lets the next one start at once, no catch-up; reset after a pause); parked on its notification while paused; netplay: waits per frame for the other panel's input (select on the socket and the link eventfd) |
| svp_game_pace (esp_timer) | one-shot, made with the first game | – | – | – | wakes svp_game at its frame's due time |

## 3. Locks (and the order they nest in)

| Lock | Taken by |
|---|---|
| `media_lock_` (S:62) | playback/draw tasks blocking; main loop try-0 only |
| `session_lock_` (S:63) | main loop try-0 (`run_pending_start_`, re-checks `start_pending_` under it); net task 10 s (RIFF start, refused while `start_pending_`/`audio_start_pending_`/`spice_on_`); spice task 3 s (`desktop_take_panel_`: `spice_on_`, `start_pending_`, ending a session) |
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
| `pool_requests_[4]` (task, user, groups) | `pool_request_()`: a task's step of the owner table (`pool_take_()`, section 9) | `pool_service_()` (loop step 2: video's buffers, outputs) and `pictures_loop_` (pictures + cover: in-flight read/decode finishes, an HA fetch is dropped or stopped, then `release_cover_buffers_` + `release_picture_buffers_`); `pool_answered_()` sets 0 and wakes | <=2 s, then taken back (set 0 by the requester: nothing freed for nobody) |
| `pic_fetch_` / `cover_fetch_` (`ChannelFetch`: generation << 3, IDLE/QUEUED/WRITING/STOP/DONE) | main loop `fetch_start_()` (QUEUED, the generation as the request's index) | channel task: QUEUED -> WRITING after the relay answered, before the first block; result -> DONE; main loop `fetch_done_()` takes it. `fetch_drop_()`: QUEUED -> IDLE (never written), WRITING -> STOP (the task ends after its block, -> IDLE) | – |
| `start_req_` 0/1/2 | playback S:1190 | loop S:291-306: prerender controls, pause LVGL, on_started, ->2 | <=2 s |
| `advance_req_` | playback EOF S:1315 | loop S:279-287 | – |
| `audio_advance_req_` | audio task S:3225, media player callback CL:101 | loop S:249-254 | – |
| `cover_job_`/`cover_decoded_` | `cover_loop_` | picture task / `cover_loop_` (polled) | – |
| `pic_decoded_`, `pic_read_.done` | picture task / channel task / storage callbacks | `pictures_loop_` (polled) | – |
| `stream_drop_sender_` | spice SP:1486 | net task R:390 | spice polls 20 ms, <=3 s |

## 5. Main loop order (`loop()` S:225-443)

1 file readers `service()` · 2 `pool_service_` · 3 finished/error/state callbacks ·
4 audio callbacks + `audio_advance_` · 5 SPICE sound claim/callbacks ·
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
| udisp stills/PCM (dashboard, desktop picture) | sender connects R:298-351 | draw task (stills; a whole frame on the display decoded into the scanout buffer not shown and flipped to at VSYNC, else into `decode_target_` and copied), net task (PCM) | not drawn while a file plays; 30 s silence ends it; desktop takeover drops it |
| Audio queue, local file | `audio_start_` S:2918 -> `start_audio_session_` S:3094 | reader (`audio_file_feed_`: `audio_file_prepare_()` reads the layout first) + audio task (`audio_file_loop_`) -> `file_speaker_`. Duration and position in ms through `duration_frames_`/`position_frame_` (`frame_duration_us_` 1000): MP3 (Xing/Info, VBRI, else CBR), FLAC (STREAMINFO), WAV (data/byte rate), AAC ADTS (mean frame size); Ogg Opus and .m4a none. A seek (`audio_seek_()`) starts the track anew there: MP3/AAC from the byte (the decoder syncs), FLAC from the next frame (header + CRC-8 checked, its sample the position) after a header of "fLaC" + STREAMINFO alone, WAV from the block after a header saying what is left; paused stays paused. on_playback_started per track (not for a seek's start), on_playback_finished when a track plays to its end | pictures keep running; video/AVI end it. `audio_session_` set from the first track to the end of the queue (`audio_advance_`), also between tracks |
| Audio queue, HA track | `audio_start_` HA branch S:2936: `audio_remote_ = ASKED`, POST /play | relay -> `svp_play_audio` -> media player | `audio_remote_player_` CL:89: ASKED+PLAYING -> PLAYING; PLAYING+IDLE (>=2 s) -> next. Any `start_playback_` sets NONE (S:669) |
| Pictures | `show_image`, next/prev, slideshow, `ha_show_pictures` | main loop + picture task; HA reads on channel task (/image) | held while a video plays (`state_`) or a stream, dashboard or desktop session has the panel (`is_network_session()`): `pictures_held_`; covers too |
| Cover art | `svp_cover` service -> `cover_wanted_` | `cover_loop_` C:121 (main loop, not while LVGL paused) -> channel task (/cover) -> picture task | buffers given back on pool reclaim (C:87) |
| Remote desktop | `spice_connect(_saved)` -> spice task (`spice_connect_browser()`: the Browser, kind `browser` to the relay, no probe; `is_spice_browser()`) | spice task; picture via relay udisp | `desktop_take_panel_` ends a video; SPICE (with sound) ends any session and the sound beside it (`sound_take_over_`: the queue's HA track, the media player's), VNC keeps an audio file unless the pool has no room for both (then it ends and the buffers are taken again); refuses media starts while `spice_on_` |
| Dashboard (relay sender of its own) | relay | net task | at the sender's start an audio file ends and `sound_take_over_` runs (main loop), so its stream PCM plays. A local video (`play()`) runs `sound_take_over_` too |
| Controls | video bar: pre-rendered at `start_req_==1` while LVGL runs, composited by playback (DSI direct only, not for network AVI). Title (the AVI's INFO INAM, else the file name; one line, cut with "...") and duration are written into the bar per file from the label's font (`lv_font_get_glyph_dsc`/`_bitmap`: font data, no widget) over the empty fields kept beside it (`ctl_draw_texts_()`), so the next file of a playlist, LVGL still paused, needs no LVGL; the time fields are as wide as "00:00:00" for every file. Not rendered yet (its memory given back): that file goes without the bar. Picture/audio bars: LVGL | main loop / playback | – |

## 7. Output paths

- **DSI direct**: only when the video equals the panel's native size (S:997-1016). One output for
  every source (video, network AVI, stream, desktop picture): `direct_target_()` / `direct_show_()`.
  A whole frame is decoded by HW JPEG into the panel's scanout buffer not shown (after the last flip,
  `wait_scanout_flipped()`, woken by mipi_dsi's frame interrupt, at most `FLIP_WAIT_MS`), the
  controls drawn into it and written back from the cache, then flipped to. Parts of a frame (udisp
  rectangles) and displays without a second buffer go into `decode_target_` and are copied in with
  `draw_pixels_at()`; the video takes `decode_target_` only then. LVGL paused by `loop()` only (S:297-331);
  per item at the start handshake: a direct item pauses it, a canvas item gives it back (only when the
  mode changes); given back once the playback task is gone and the state is STOPPED/ERROR or an
  audio-file session runs (it takes no picture), not while a direct sender streams or a playlist
  advances. The dashboard (direct sender) and a remote desktop run only with everything else stopped;
  pictures and audio are the only things that run side by side.
  No PPA in the video paths (the screen capture has a PPA client of its own, section 11).
- **Camera full screen** (`camera_direct_start/stop`, `show_camera_frame_direct()`): the panel taken as a direct
  stream is (loop() pauses LVGL, `cam_direct_ready_` once it let go); the picture task draws each frame into the
  scanout buffer not shown (`direct_target_()`), 1:1 in the panel's orientation (the camera mounted to match),
  centred and cut to the panel, black around (each buffer cleared once per picture size); MJPEG with rows of the
  panel's width decoded straight into it, else into the camera scratch and copied 1:1 by screen capture's PPA client
  (YUV converted), then flipped (`direct_show_()`). A tap calls `add_on_camera_direct_tap_callback`. Never scaled.
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
| Router format | players only `set_audio_stream_info` | the output-list resampler starts its output with the first sound, and starts again (stop, wait until stopped, start) when a player's stream format differs from the one it was started for (resampler_speaker.cpp `play()` / `loop()`); the player never starts/stops |

A/V sync: `audio_latency` (BT delay; device YAML: only while the media output is Bluetooth), read at
each start, clamped to `max_audio_latency_ms_`: what the pool's ring room leaves beside the prebuffer
(`latency_limit_ms_()`, recomputed by `set_prebuffer()`; the device YAML gives it to
`bt_out.set_max_output_delay_ms()`, the BT delay slider's and number's most). The session's ring and
sound block are sized for this lead; sound released when the cursor leads by `audio_lead_frames_`
(the cursor peeks the ring that far ahead); picture clock anchored that much later.
`audio_preroll` (device YAML: the a2dp buffer while the output is Bluetooth, else 0): at a feed's start (playback,
resume, after a seek) the audio task writes silence to `speaker_` so that the feed's first sound has that much in front
of it in the speaker chain - minus what of its own sound is still in the chain (`sound_written_` against
`sound_played_`, the speaker's audio output callback; `sound_base_` taken at the playback's start). The lead is
audio_latency + audio_preroll (clamped together), so the picture stays with the sound and the chain keeps that much
sound ahead of what it plays; without it the cursor writes the sound only about one video frame ahead of what a
stream started at the sound's release plays. Pacing stays on the wall clock.

## 9. PSRAM pool and buffers (today)

Pool: `psram_pool_size` (default 12 MiB, 6-28; device 15 MB = 15,000,000). Plan in `plan_pool_()`
`plan_pool_()`: everything listed as "fixed" is reserved; the rest (`ring_room_`) is the read-ahead
ring's and the sound block's. A video takes the ring for `prebuffer` + its lead at `MAX_MEDIA_RATE`
(4 MiB/s, whole 256 KiB chunks, at least two) and the sound block for its lead; both are kept and taken
anew only when a session needs another size (`acquire_session_buffers_()`, `sound_block_for_()`). The
start fills the whole ring (`prefill_cache()`), so its size is also the start's wait. `prebuffer`
(YAML, default 500 ms, min 100 ms; device: number "Video prebuffer", Settings > Outputs) is kept within
what the room leaves (`max_prebuffer()`).

| Buffer | Size | Taken / returned | Content | In plan |
|---|---|---|---|---|
| `cache_buffer_` | 64 KiB | setup, never | raw reads, index/audio scratch | yes |
| sound block (one ring over it: input or decoded) | max of the configured kinds: input rate·ch·2·(lead+250 ms) >=32 KiB, decoded rate·ch·2·max(lead+250, 500 ms) | setup for no lead; a video with another size takes it anew | compressed audio / PCM | yes (ring room) |
| `audio_temp_buffer_` | max(BPS/10, 8 KiB) | setup, never | PCM drain / file read | yes |
| audio-file ring | 128 KiB | setup, never | compressed audio file | yes |
| `stream_recv_buf_` | 32 KiB | setup, never | TCP bytes | yes |
| `stream_frames_[4]` | 4 × frame_bytes_/4 | setup, never | compressed JPEG stills | yes |
| `stream_audio_block_` | 10 ms PCM | setup, never | PCM | yes |
| stream input queue | 384 B | setup, never | input messages | no |
| SPICE state/saved/file | ~8 KB | setup, never | config | yes |
| keyboard list + entries + assignments + 6 layouts | 32 KiB + 6 KiB + 24 × 116 B + 6 × 8 KiB | setup, never | relay's layout list (cut in place), who has which layout, the layouts in use (cache, KBD_SLOTS) | yes |
| `thumb_buffer_` | stride·th·count (~216 KiB default) | setup, never | decoded thumbnails | yes |
| `frame_slots_[1]` | frame_bytes_ = clamp(pool/24, 256 KiB, 1 MiB) | first session, kept | compressed frame | yes |
| `index_buf_` | clamp(pool/32, 128 KiB, 1 MiB) | first session, kept | AVI index (also allocated for streams) | yes |
| `ring_buf_` | 4 MiB/s · (prebuffer + lead) (device: 2 MiB without BT delay) | first session, kept; taken anew when a session needs another size | compressed AVI/MJPEG | yes (ring room) |
| `decode_target_` + `ctl_mem_` | panel w·h·bpp (2.05 MB) + 3 × bar snapshot + the empty title/duration fields (bar width × line height each) + 4 KiB | first direct output, kept | decoded frame, controls | yes (max of direct/canvas) |
| `output_buffers_[2]` | 2 × A16(cw)·A16(ch)·3 | first canvas output, kept | decoded RGB888 | yes (max of direct/canvas) |
| `pic_pages_[2]` | 2 × stride565(max(disp, canvas)) · h | first picture, reclaim, `close_picture()` | the shown picture and the next one (no decode ahead; neighbours only read undecoded) | **no** |
| `pic_decode_buf_` | the decode's size in 1 MiB steps | only while a picture larger than its target (or a thumbnail) is decoded | full native decode | **no** |
| `pic_files_[3]` | 1 MiB each, grows | first picture, reclaim, `close_picture()` | compressed files | **no** |
| page background `bg_data_` + `bg_file_` (`background_canvas_id`, `SVP_BACKGROUND`) | A16 w·h·2 of the canvas (device 1280x800: 2,048,000) + the file (<= 4 MB, read only) | `bg_data_` while the page shows (`set_background_wanted`); the file and, for a JPEG larger than the canvas, a decode piece of its own (`bg_scratch_`) only for the read/decode (the viewer's pages/files untouched); both back on leaving the page and on the pictures' reclaim | decoded background (fitted as pictures, `decode_job_` on the picture task) | **no** |
| `cover_views_[<=3]` + `cover_file_` | A16 w·h·2 each (device 25 K + 320 K + 1.28 MB) + 512 KiB | cover wanted, reclaim | decoded cover / JPEG | **no** |
| desktop cursors | ~300 KiB | session start, `release_desktop_` | cursor shapes | no |
| USB transfers per slot | ~550 KiB at link_open + ISO | link open, `release_buffers` | USB data | no |
| capture pictures `cap_frame_[2]` | LVGL screen w·h·bpp in whole cache lines (device 2,048,000 each) | screenshot: one, till its file is written; screencast: two, its start to its end | what LVGL shows | **no** |
| capture JPEGs `cap_jpeg_[2]` | 512 KiB each | with the pictures | prefix 128 (the chunk header before the JPEG) + JPEG + the batch's further chunks and sound | **no** |
| screencast index `cap_index_` | 128 KiB | screencast's start, after its file is complete | ix00/ix01 of the segment (60 KiB each), 8 KiB super index entries; the header built into it | **no** |
| screencast sound ring `cap_pcm_` | 32 KiB | screencast with sound | the microphone's PCM | **no** |

Handover: the owner table (`POOL_OWNERS`, H). Every user takes from the pool; only when a piece does
not fit are other users' idle buffers handed back, group by group in its row's order, and only while
it still does not fit (`pool_take_()`). Groups: pictures (+ cover art), video (ring, slot, index),
direct (decode buffer + controls), canvas (pair), desktop (cursors, USB), capture (the screencast's
or a screenshot's pictures and JPEGs: `capture_give_back_()` in `pool_service_()` ends the
screencast and answers once they are back; its index and ring go when its file is complete).

| User | Steps |
|---|---|
| video (playback: ring, sound block, slot, index) | capture, pictures, desktop |
| direct / canvas output (playback) | capture, the other output, pictures, desktop |
| direct / canvas output (stream draw: dashboard, desktop picture) | capture, the other output, pictures, video, desktop |
| pictures and cover art (main loop) | outputs, video, desktop; only when nothing plays or streams |
| desktop (spice task: cursors, USB) | capture, pictures, video |
| capture (main loop: screenshot, screencast) | video, outputs (only idle: nothing plays or streams); never pictures |
| game (main loop: ROM, the core's pieces, the PPA frame; section 12) | outputs, video, desktop |

The video's buffers go only while no video session runs or starts (`video_session_()`); the stream
never takes a file's output. A task's step is a request to the main loop (`pool_request_()`); the
desktop's own buffers are given back by `release_desktop_()` on the asking task. The session start
takes ring, sound block and slot in one try from the pool (`take_session_buffers_()`); one not
fitting, what the try took goes back before the step, so the ring finds one free stretch. An HA
picture or cover fetch in flight is dropped (not writing yet) or stopped after its block
(`fetch_drop_()`), so a video start never waits for it.

Objects of svp's own, a pool piece each from the pool's end (`alloc_top`: they never split the media
buffers' stretch), counted in `plan_pool_()` (`psram_make`, `PoolAllocator`):
file readers, AVI parser, PNG/BMP decoder, the sound ring's object (with its shared_ptr block), the
audio decoder with its source, transfer buffer (and its data) and codec objects (`audio::AudioHeap`).
The lists (`pool_list.h`): a `PoolList` of fixed capacity each, its entries `FixedString`s kept in
place, a piece from the pool's end taken in setup for good, counted in `plan_pool_()`; full: refused;
a path/id too long: left out (logged), a title: cut. Taken only for the features configured:
`playlist_` 500 × 256 B (+ 500 × 2 B index for sorting); the audio queue (file speaker) 500 × (384 + 128 + 2) B; pictures (canvas)
`images_` 1000 × (384 + 128 + 8) B (+ 1000 × 2 B index; `sort_by_order`: the indexes sorted, each
entry moved once), thumbnail slots/queue per thumbnail; channel list (`url`, with
`HaTexts`: the folder listing's texts ×2, play/picture ids) channels 4 × 300 × 64/256 B, HA folders
2 × 500 × (128 + 384) B, sources 32 + a ring of 3 × 32, listings asked 8, relay settings 32/16 (×2),
8 sets; keyboard layouts: downloads 16 × 32 B, `keyboard_devices()` 32 (one list, made anew per
call). The channel task writes the "new" list outside `channel_lock_` after clearing its flag under
it; the main loop takes it only with the flag set, checked under the lock. Local folders (a picture folder, a
video's folder as its playlist) are listed by the storage worker (`async_list_dir`; synchronously
without one or with its queue full): its entry callback writes the list's places past its entries
(`PoolList::place`), the completion on the main loop takes them in (`adopt`), sorts and shows; a
listing asked for meanwhile drops the one on its way and lists anew (`FolderListing`, a pool piece).
The video plays at once, its folder's videos put round it when listed. A picture folder's failed
listing leaves `images_` empty (`picture_listing_failed()`, the device YAML shows "Listing failed"). Still on the PSRAM heap: the configuration's lists filled before
the pool exists (picture folders, controls buttons, keyboards). Stay outside by decision:
the codec libraries' own working buffers (FLAC_MALLOC etc.) -- ESPHome's audio
codecs as they are, in PSRAM.
Pool, per desktop session: `spice_scratch_` 64 KiB PSRAM, taken by the svp_spice task when a connection is
asked for (before the SPICE / VNC probe) and given back when the session ends.
Outside the pool (svp): per USB slot bookkeeping; the slot's 4 KiB internal `stage_` is part of its session block
(internal, made at the redirection's `link_open()`, freed with the session's transfers); task stacks 48 KiB PSRAM; the configuration's PsramVector lists (above); `channel_http_` response buffer PSRAM 16-256 KiB per request; AudioDecoder internals;
PNG/BMP decode (RAMAllocator); LVGL objects; mbedtls per SPICE connect.

## 10. Home Assistant / relay interface (summary; details in protocol.md)

- HA services (only with `media_player_id`, `api: custom_services: true`): `svp_play_audio`,
  `svp_audio_command`, `svp_set_volume`, `svp_cover`, `svp_stream_statistics`.
- Status text sensor JSON (MS:41-71): `svp`, `port`, `state` (media player's), `volume`, `muted`,
  `playlist`, `video` (`local_video_active()`: a video file of its own PLAYING/PAUSED, not the relay's AVI,
  not an audio file; republished when it changes), `pause`, `formats`.
- Panel -> relay HTTP on the channel task: `/channels`, `/play`, `/stop`, `/browse`, `/image`,
  `/cover`, `/options`, `/desktop`, `/keyboard`. The first `/channels` and every `/browse` asked for
  before the network is up wait in `channel_loop_()` (`ha_browse_waiting_`) until `network::is_connected()`.
- Stream port: AVI ('RIFF') or udisp (16-byte header); panel reports 'C','V','A','L','N','F','S'.

## 11. Screen capture (screen_capture.cpp, `SVP_CAPTURE`)

- Pictures come from LVGL's drawing: `LvglComponent::FrameCapture` (lvgl_esphome.h). In `flush_cb_` each
  rendered area is also copied (LVGL's PPA client, angle 0; else memcpy + cache write-back) into the
  buffer `capture_target()` gives, in the LVGL screen's size, orientation and colour format (device
  1280x800 RGB565); `capture_done()` at the refresh's last flush. `copy_shown()`: the shown scanout
  buffer turned back (PPA) into that orientation.
- Screenshot (not casting): one picture from `copy_shown()` (else LVGL draws the screen anew into it:
  `cap_single_`), encoded on svp_capture, written by the main loop (storage worker), its buffers given
  back. Casting: the newest JPEG written out (its buffer kept by `cap_jpeg_users_` till written).
- Screencast: two pictures. LVGL draws into the back one; at `capture_done()` with something drawn and
  the encoder not reading the front one (`cap_owner_` FREE -> SWAPPING), they swap and svp_capture
  copies the changed areas (`cap_sync_rects_`, its own PPA client) into the new back one; LVGL's next
  `capture_target()` waits for that (`cap_sync_sem_`, at most 100 ms, else the screencast ends). The
  tick (esp_timer, 1/fps): the front picture encoded (ENCODING) when it changed since, into the JPEG
  buffer not written out; a batch (`CastAvi::Batch`: the newest JPEG, else an empty chunk = the same
  picture, one per tick since the last batch, and the PCM since) composed once the last batch is out,
  to the file (main loop) and/or svp_relay (svp_cast_tx). No encoding of anything else.
- The file: header (sizes 0) at the start; batches appended; a segment ends at 1 GB or a full index:
  ix00, ix01, the header again (sizes, super indexes), an AVIX segment's start patched, the next
  segment's start. At the end the same, then closed. 256 segments at most (ends with a notice).
- svp_relay: POST /cast on the channel task (protocol.md), the cast connection on svp_cast_tx: the
  header and every batch as the file gets them (no indexes, sizes 0).
- Not while a video, a stream sender (dashboard) or a remote desktop has the panel; one of them starting
  ends it (`capture_loop_()`, and POOL_CAPTURE in their rows), with the reason in
  `on_screencast_stopped` (the device YAML shows it as a notice). Starting is refused the same way, and
  when the pool has no room (the capture row hands back only idle video buffers and outputs).

## 12. Game emulator (emulator.cpp, emu_cores.cpp, emu/, `SVP_EMULATOR`)

- An app (`AppKind::GAME`): `app_take(GAME)` ends a video, a desktop session, pictures and the camera; the audio file
  session and the media player's sound stop. While a game runs or is paused (`emu_on_`) every other start is refused
  (`emu_refuses_()`: `play()`, `play_index_()`, `audio_start_()`, `app_take()` of another app, SPICE / VNC connects,
  stream senders, `svp_play_audio` without announce).
- States (main loop): LOADING (waits until no video session runs, stat, the ROM's pool piece, worker read stream
  straight into it), core load (main loop; `on_started` once loaded, before the panel), WAIT_PANEL (`wants_panel()`: `loop()` pauses LVGL as for the camera;
  `panel_ready()` once LVGL let go), RUNNING (svp_game), PAUSING / PAUSED (the menu key; the task parks, the panel goes
  back to LVGL, `on_paused`), STOPPING (the task ends, `StaticTask::deallocate()`), then everything is given back
  (`on_ended`).
- Memory: the cores' `malloc` / `calloc` / `realloc` / `free` (forced `emu_force.h`) go to `GameEmulator`: a piece internal
  (at most 16 KiB, a hot piece any size) while the system's internal budget lasts (NES 24 KiB, Game Boy 24 KiB, SMS
  25 KiB, SNES 96 KiB,
  Mega Drive 14 KiB, PC Engine 8 KiB, Lynx 12 KiB, others 0; SNES, Mega Drive, PC Engine and Lynx only for the pieces
  their cores ask for with `emu_calloc_hot` / `emu_new_hot`: Lynx handy's CSystem, CMikie, CSusie and C65C02 objects
  (its 64 KiB CRam stays in the pool), PC Engine its work RAM `PCE.RAM` (zero page and stack), SNES
  in this order its state structs (`S9xInitHot()` before the core starts: CPU, ICPU, APU, IAPU, SoundData, Settings,
  Memory, PPU, IPPU, `S9xDMA`, `S9xBG`, GFX, 6.8 KiB; pointers in `.bss` behind macros of their names, `BG` and `DMA`
  renamed as `PPU.BG` is a field), the 64 KiB VRAM (one block; without a free 64 KiB block it goes to the pool), the
  memory map `Map` and `MapInfo` and the palette `IPPU.ScreenColors`, 95 KiB together, Mega Drive the Z80 RAM `ZRAM`, the YM2612's `sin_tab`, the VDP's `SAT_CACHE`
  and `CRAM565`, allocated by its port before the core starts),
  else the pool (`POOL_USER_GAME`, owner row: outputs, video, desktop); every piece in a table of 96, all given back at
  the end. The cores' hot structs (nofrendo: nes, cpu, mem with its RAM as a pointer, ppu, apu) are allocated in their
  init functions through it (gnuboy: GB, cpu, snd, WRAM / VRAM per bank in the order VRAM 0, WRAM 0, WRAM 1, VRAM 1;
  smsplus: sms, vdp, Z80; handy's `new` through `emu_new`). ROM, the PPA's RGB565 frame and everything larger come from
  the pool.
- Cores (`systems:` -> `CONFIG_SVP_EMU_<SYSTEM>`): nes (nofrendo), gb (gnuboy, GB / GBC), sms (smsplus: SMS, Game Gear,
  SG-1000 `.sg`, ColecoVision `.col`: keypad 1 / 2 / * / # on X / Y / L / R, SELECT resets), pce (pce-go, PSG rendered
  per frame in chunks of 64), lynx (handy, 32 kHz, the cartridge's rotation), snes (snes9x 2005, all PSRAM, mixed per
  frame in chunks of 660 (its mix buffer), one SNES frame per call (a DMA's line catch-up in
  dma.c stops at the frame's end, the rest follows in the next frame), the ROM piece 64.5 KiB larger for its mapping), md (gwenesis, YM2612 at 26,633 Hz, PSG off as in retro-go, the ROM piece in
  whole 64 KiB), gw (gw-emulator, LCD-Game-Shrinker `.gw` files, two 128 Hz steps per frame, 32,768 Hz, watch mode
  started and the clock set from the system time as in retro-go), msx (fMSX, MSX2 with 2 RAM / 2 VRAM pages: the
  core runs its own loop on svp_game and calls `emu_host_frame()` from `Keyboard()` once per frame, where the picture,
  the frame's sound, pause and stop are handled; `bios_dir:` files MSX.ROM, MSX2.ROM, MSX2EXT.ROM, MSX2P.ROM,
  MSX2PEXT.ROM, DISK.ROM, MSXDOS2.ROM, FMPAC.ROM, KANJI.ROM, PAINTER.ROM, RS232.ROM, CMOS.ROM are read after the ROM where
  present and served with CARTS.SHA from memory (`fmemopen`), writes are refused; keyboard keys go to the MSX
  keyboard matrix, the pad to joystick 1 and to cursor / space / enter / escape; `.dsk` as drive A). A frame without
  sound plays one frame of silence, so every frame has its sound and its due time.
- Picture: the largest whole factor that fits LVGL's screen, centred, black around (each scanout buffer cleared once
  per game and after a pause); turned only when LVGL's rotation is not 0 (PPA SRM: LVGL 90 -> PPA 270, 180 -> 180,
  270 -> 90); factor 1 at rotation 0 draws without the PPA; PPA errors fall back to the CPU. Own PPA client (registered
  per game, core 1 like the other DMA2D users).
- Sound: the core renders in `wanted_audio_rate_` (8-48 kHz, else 32 kHz) and `wanted_audio_channels_`;
  `set_audio_stream_info` once per game; the speaker is never started or stopped.
- Keys: `GameEmulator` is LVGL's `PausedInput` while it wants the panel (else svp's stream taker again,
  `paused_input_default_()`); `keys:` per button (hid key names, defaults positional pad buttons + keyboard),
  `menu_keys:` (default BUTTON_11) pauses; every key is taken, the configuration's own key_bindings actions still fire.
  Keyboard keys go to a core with a keyboard (MSX) instead of the button map.
- Players: every key event carries its device (hid `key_source()`: the device-level HIDInput, forwarded by usb_hid's
  client and bt_pairing; `key_source_label()` its name; LVGL queues it and passes it to `paused_key`). With `hid_ids:`
  and two or more key devices connected (`players_wanted()`), the page opens the Players dialog
  (`emulator.players` path): LVGL's KeyTap gives every key to the emulator (a ring of 8, drained in `service()`): a
  device's first key adds it (not playing), its left / right move it to player 1 / 2, A or START of a playing device
  starts the game, B or a menu key cancels; touch through `emulator.players_set` (index, seat 0-2),
  `emulator.players_start` / `players_cancel`; triggers `on_players_changed` / `on_players_closed`. A game started so maps
  each device's buttons to its player's pad; one started without the dialog maps all to player 1. Opened while a
  game is paused, Start maps the devices anew and the game stays paused.
- Saves (`<rom without extension>.sav` and `.st1`..`.st4` next to the ROM): the cartridge RAM (nes, gb, sms, snes:
  `sram()`) is read from `.sav` before the panel is taken, and written while paused and at the end when its CRC32
  differs from the last read / write (a worker write stream straight from the core's memory; the end waits for it).
  States only while paused: the core serialises into the borrowed storage transfer buffer (at most 768 KiB; the cores'
  stdio state code goes to `emu_mem` by compile definitions: nofrendo state.c, gnuboy.c, smsplus state.c, pce-go.c,
  snes9x snapshot.c, handy; gwenesis, gw and fMSX are memory-based already), written / read by the worker; one job at a
  time, a resume asked during a job runs after it. `slot_time(slot)` stats a slot for the page (mtime, 1 when unknown).
- Link (`link:` port, default 6666, `peers:` host names for the page; `SVP_EMU_LINK`, VFS select): `emulator.link_host`
  / `link_join` (host) / `link_off` choose the next game's link (taken at its start). After the core load (and the
  `.sav` read) state LINKING makes svp_game early: the host listens (until a guest comes or stop), the guest resolves
  and connects (5 s); one plain TCP socket (`TCP_NODELAY`, keepalive, 5 s send timeout), messages of 8 bytes. Both
  send a hello (version, system + cable flag, the ROM's CRC32) and play alone when it differs or does not come. The task
  then parks, the main loop takes the panel as usual. Waits are `select()` on the socket and the link eventfd, which
  `stop()` and `pause()` write: no polling, no timer. Netplay (every system without a cable): each frame sends the local
  pads (all local devices together) for the next frame and waits for the other panel's pads of this frame (one frame
  of input delay, host = player 1, guest = player 2; 5 s without input ends the link, a paused panel announces its
  pause and the other waits without the timeout). Cable (Game Boy / Color, Game Gear, Lynx; each panel runs its own
  game): shim `emu_set_link()` / `emu_link_send()` / `emu_link_recv()`; gnuboy sends the master's byte at the `RI_SC`
  start and completes the transfer with the other panel's byte (300 ms, else 0xFF), a waiting slave answers with its
  `SB` (polled every 1824 cycles and once per frame); smsplus sends `txdata` with TON and queues 16 received bytes,
  delivered while RON is set (RXRD, NMI with 0x08); handy's ComLynx TX callback sends, received bytes go to
  `ComLynxRxData` once per frame. Link loss (error, close, bye, timeout) ends the link, the game goes on alone (no
  reconnect); status `link` ("off", "waiting", "connected", "lost"). The sockets (claimed: 1 TCP, 1 TCP listen) and the eventfd exist only
  while the linked game runs.
- Actions: `simple_video_player.emulator.start` (path) / `.stop` / `.pause` / `.resume` / `.save_state` (slot) /
  `.load_state` (slot) / `.link_host` / `.link_join` (host) / `.link_off`; Home Assistant `svp_emu_start` (path),
  `svp_emu_stop`, `svp_emu_pause`, `svp_emu_resume`, `svp_emu_save_state` (slot), `svp_emu_load_state` (slot),
  `svp_emu_host`, `svp_emu_join` (host); the status JSON's `game` ("", "running", "paused") and `link`.
