# svp open issues (2026-10-16)

Line references (S:, P:, CT:, CL:, R:, U:) were taken on 2026-10-15; the code has moved since, search by name.

Status: **REPORTED** = Olli observed it on the device · **CODE** = confirmed by reading the code ·
**PLAUSIBLE** = likely from the code, not proven. Nothing here is fixed unless Olli ordered it.
Remove an entry only when Olli confirms the fix on the device.
Only what Olli names goes on this list (Olli, 2026-10-16). REPORTED issues always come first.

## A. Bug reports from Olli

| # | Report | Notes |
|---|---|---|
| A1 | **Crash: opening a picture from an HA source while audio from an HA source plays.** Works: local audio + pictures from any source; pictures first, then audio. REPORTED | Unanalysed. Difference to local audio: the HA track brings cover art (pool buffers, channel task /cover fetch, picture-task decode) and the media player's HTTP stream. Check pool state and the cover/picture interplay on the channel and picture tasks first |
| A2 | (changed 2026-10-17: device YAML `browser_build_ha` and `browser_build` (local folders, `browser_local_slice`) make the first 40 rows, `browser_more` the rest 40 per loop pass while LVGL runs and the Media page shows that folder; `media_init` one loop pass after the page, the queue list once; a type or source change makes the list a loop pass later (`browser_refresh_later`), the old rows hidden and deleted 40 per loop pass (`browser_drop_old`); a local folder read by the storage worker (`async_list_dir`, `Loading...` meanwhile, synchronous only without a worker or with its queue full); awaiting Olli's test) **HA media list loading blocks the main loop**; must be asynchronous. REPORTED | Located (2026-10-16, code): fetch and JSON parse run on the channel task (`ha_browse_task_`); `ha_loop_` (CL:217) only swaps the lists. The block is the UI build in one loop pass: `ha_browse_callback` -> YAML `ha_listing` -> `browser_refresh` -> `browser_build_ha` runs `browser_add_row` (a `kit_row`: button + icon + label, styles) for each of up to `HA_ITEMS_MAX` 500 items, then `kit_swap` deletes the old rows and `browser_mark` walks them all. Same pattern for local folders (`browser_build`, up to 400). Fix needs a design decision (build in slices over several loop passes, or a list that only creates the visible rows); not changed |
| A3 | (changed: number "Slideshow picture duration" 2-60 s, on the Pictures controls, the full-screen picture bar (slide_time) and Settings > Media folders; awaiting Olli's test) "Diashow-Bilddauer" as a setting (UI + HA). Open question | Firmware has `set_slideshow_interval`; YAML only the `ui_slideshow_seconds` substitution. Olli's original list must be resent |
| A4 | SPICE folder sharing (WebDAV port channel); decide on the VNC file-transfer relay code (rfb_files.py) | TODO after remote tests |
| A5 | Cover art of local audio files via an art-db | Own topic |
| A7 | **HA audio in the queue starts with a long delay**; the same media started from HA the usual way on the device starts at once. Olli's proposal: the queue only remembers the track and triggers each one through the usual HA/ESPHome way, no own path. REPORTED | Design change; scope with Olli first |
| A8 | (changed: lead max from the pool room the prebuffer leaves (2026-10-17, B1), given to the BT delay's slider and number; YAML applies the BT delay only on the Bluetooth output; awaiting Olli's test. The lead is read once per session start: output switch during playback is a later special feature, after everything more important works (Olli)) **BT audio delay (`audio_latency`) is wrong**: a) doesn't work correctly, b) makes USB speakers worse, where it must not apply at all. REPORTED | Device YAML lambda uses `bt_out` delay; C++ clamps to `max_audio_latency_ms_` (S:680). Scope with Olli first |
| A9 | **Rename the svp components** (svp and its sub-projects, now far more than the video player). TODO, deliberately later: together with the review/refactor phase once the functionality is there (Olli) | Not before then |
| A10 | (changed: keyboard layouts per keyboard (YAML `keyboards:`) and per device (USB `usb:vid:pid[:serial]`, BT `bt:MAC`), from Settings > Devices > Keyboards and HA (`svp_keyboard_layout`, sensor "Keyboard layouts"); layout folder settable (YAML `directory:`, Settings > Media folders, text "Keyboard layout folder"); "New folder" in the folder chooser; several downloads at once; relay layout sources as panel options, install only from the panel (relay 0.9.41); awaiting Olli's test) Crash: OSK opened from search, then the Keyboards test field used (dangling `lv_keyboard` ta). REPORTED. 2026-10-16: the OSK's own on_ready/on_cancel triggers read `lv_textarea_get_text()` on the keyboard object itself (ESPHome `widgets/keyboard.py`), past its end, and could crash on hiding it; fixed in 6f8f70eb81 (lvgl), may be this crash too | Open: bundled 745 layouts in the relay zip keep or drop; `storage_options_fill` vs. an open list (freed options); SPICE guest layout (per connection or global) |
| A11 | **Crash: opening Settings > SVP Relay** (2026-10-16). REPORTED | Not found in code (2026-10-16, two passes, no log): the section 11 build (`settings_fill` case 11), its choice/slider/textarea helpers, the relay option data (0.9.42/0.9.43), `opt_values_` threading and string lifetimes, deletion during events (LVGL 9.6 `obj_delete_core` removes event callbacks first) are ruled out. Unchecked: main task stack depth (8192) while building the section. Needs the backtrace. PLAUSIBLE (2026-10-16, not proven): reported while the device YAML lacked the SPIRAM malloc options (plain malloc internal only; the same day's log showed esp-aes/TLS allocation failures); LVGL itself allocates from PSRAM (`lv_malloc_core`, `lvgl_esphome.cpp`), the section's `std::string`s and the channel task's HTTP client do not. Re-test with the options restored first |
| A12 | Relay options: `sample_rate` and `channels` got no choices (relay: `SAMPLE_RATES`/`CHANNEL_MODES` are dicts, `_choices` took lists only) so the panel offered false/true and setting one answered HTTP 500 (KeyError); the panel dropped an empty first choice (`default_output` "as it is"). Fixed: relay 0.9.43, bb532a6687; awaiting Olli's test | Labels on the panel are the raw values (device, prefer_48000, ...) |
| A13 | Header icons: rework; already very good, not yet perfect (Olli, 2026-10-16). TODO | Details from Olli; design first |
| A14 | **BT audio: reconnect initiated by the device does not work correctly** (everything else does). REPORTED 2026-10-16 | a2dp_source; do not act alone: analyse, then scope with Olli |
| A15 | On-screen keyboard settings, most important: "show the on-screen keyboard even if a physical keyboard is connected" (default no); in LVGL settings and reachable from HA. TODO 2026-10-16 | Scope with Olli first |
| A6 | TV regression 2026-10-15 fixed by 6a5877bba8 (cover on demand + reclaim before a video's buffers). Which of the two fixed it is not proven; likely the reclaim (no holes for the ring) | Keep in mind for B3 |

## B. Rule violations (memory / buffers)

| # | Issue | Status |
|---|---|---|
| B1 | (changed 2026-10-17 per Olli: the ring holds the prebuffer (YAML + LVGL + HA option, default 500 ms) plus the session's sound lead, at 4 MiB/s; the sound block the lead; both taken anew only when a session needs another size; the lead's and the prebuffer's most come from the room left; awaiting Olli's test) **The ring takes the whole rest of the pool** (S:2128-2146), and audio latency + audio rings derive from it. Raising the pool only grows the ring; nothing sized by need | CODE |
| B2 | (changed in the commit after 775e5ff: 2 pages, neighbours undecoded, decode buffer on demand; awaiting Olli's test) **Pictures pre-decode**: 3 decoded pages (P:519) + a kept native decode buffer (grows, P:563-577) + 3 compressed files; the same picture compressed and decoded at once | CODE |
| B3 | Pictures and cover are not in the pool plan; they live off whatever the ring leaves or by freeing all media buffers (`release_media_` is all-or-nothing, P:501, C:61). Fragmentation possible when pieces stay inside the free area | CODE / PLAUSIBLE |
| B4 | (Olli 2026-10-16: the relay keeps delivering each view's cover at its size, fetched one after another, the view shown at that moment first: changed 2026-10-16, awaiting Olli's test. Changed too: the cover buffers counted with the pictures as the picture + audio mode's share, checked at setup against the media room it takes over (error log if it does not fit); handed over on the mode switch as before; the JPEG file buffer is kept and reused for the next cover) Cover: all 3 views are decoded and held while cover art shows, though only the visible one is needed; `cover_file_` (512 KiB) kept after decode | CODE |
| B5 | (changed 2026-10-16 per Olli: one direct output for every source, `direct_target_`/`direct_show_`; a whole frame is decoded into the scanout buffer not shown and flipped to, the wait for the last flip woken by mipi_dsi's frame interrupt (`wait_scanout_flipped`); `decode_target_` only for parts of a frame and one-buffer displays, the video no longer takes it; awaiting Olli's test) DSI direct: decoded frame in `decode_target_` (2.05 MB) is copied into the panel framebuffer every frame (S:1402): a full frame twice. Verified 2026-10-16: the panel's framebuffers take the decoder's output; the udisp stream path already decodes into the scanout buffer not shown and flips at VSYNC (`stream_flip_target_`, mipi_dsi `get_scanout_buffers`/`present_scanout_buffer`/`scanout_flipped`); device has `frame_buffers: 2`. The video path does not | CODE |
| B6 | (changed 2026-10-16 per Olli: one sound block, as large as the larger ring kind the configuration uses (input ring: audio_codec mp3/flac or audio files; decoded ring: pcm); the ring over it is the kind the file needs, made anew over the same block only when the kind changes (`sound_ring_for_`); temp buffer only for pcm or audio files; awaiting Olli's test) One of the two audio rings is always idle: PCM uses only the decoded ring, MP3/FLAC only the input ring; both held for good (S:2577, 2638). Decoded PCM ring conflicts with "undecoded only" | CODE |
| B8 | (changed 2026-10-16: taken in `open_file_` only for a file's AVI, kept with the ring; awaiting Olli's test) `index_buf_` is allocated for network AVI and raw MJPEG, which use no index | CODE |
| B9 | Controls: 3 × bar snapshot reserved (CT:331-342); check need | CODE |
| B10 | (partly changed 2026-10-16: `PsramPool`'s piece table is a fixed table of 128 at the pool block's start, no internal heap growth; a full table hands a free piece out whole) Allocations past the pool: `channel_http_` response buffer (PSRAM realloc up to 256 KiB per request, CL:578); PNG/BMP decode via RAMAllocator; `PsramPool::pieces_` grows on the internal heap past 32 pieces | CODE |
| B11 | (partly changed 2026-10-16: an output request and the desktop's media request are taken back after the wait, the main loop clears only the output request it served; the pictures' reclaim request is not, two tasks may share it) `wait_alloc_answer_` expiry: after the 2 s timeout `pool_reclaim_req_`/`output_release_req_` stay set; the main loop frees later for nobody (H:2110-2117) | CODE |

## C. Rule violations (speakers, LVGL, hot path)

| # | Issue | Status |
|---|---|---|
| C1 | `file_speaker_` (audio queue resampler) is still stopped/finished/paused by the player (S:3194, 3230, 2942, 3086, 2861). Speaker rule. Open question to Olli: a resampler takes its input format only at start | CODE |
| C2 | (changed 2026-10-16: the thumbnail strip's auto-hide and the channel dropdown's new list (`channel_loop_`, `set_options`) wait until LVGL runs (a channel list that changes during a direct stream is applied after it; indexes from the relay's new list meanwhile meet the old one); awaiting Olli's test. Also found: the `pool_reclaim_req_` block in `pictures_loop_` runs before the held check and can call `picture_decoded_()` -> `show_page_` (canvas buffer, sizes, styles, invalidate) while LVGL is paused, e.g. a reclaim for the next playlist video or the desktop) LVGL touched while paused: `prerender_controls_` for the next playlist item while LVGL is still paused (S:294 vs 327); `slide_thumbnails_` auto-hide before the held check (P:141); `lv_obj_is_hidden` on the picture bar every loop (CT:904); pictures guarded only by `state_` (udisp/desktop sessions pause LVGL with state STOPPED); `set_options` (CL:207), `lv_image_set_src` in `handle_output_release_` (S:2311) / `release_media_` (S:2364), `release_picture_buffers_` (P:586) without a pause guard | CODE |
| C3 | Two different "paused" guards in use: `lvgl_component_->is_paused()` vs `dsi_lvgl_paused_` | CODE |
| C4 | (changed 2026-10-16: "Dropped ... incomplete audio frame" counted in `audio_dropped_bytes_`, one log at the session's end in `end_session_audio_()`; awaiting Olli's test) Logs in hot paths: `ESP_LOGW "Dropped %zu bytes"` per audio chunk unlimited (S:2628); audio decode error repeats (S:2823, bounded by `AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES`); `ESP_LOGE` pool no-room per stream frame + 2 s wait per frame when the output can't be had (S:2165, R:1037/1049; part of the buffer manager, G7, not changed); usbredir skip per packet (U:678, only for non-data packets over `STAGE_BYTES`, rare; not changed) | CODE |
| C5 | (changed 2026-10-16: `desktop_take_panel_` waits on the spice wake fd, woken when the sender / the session's tasks let go, instead of a 20 ms loop) Polling: cover/picture completion flags polled every loop; `vTaskDelay(20)` loop in `desktop_take_panel_` (SP:1496); `vTaskDelay(1)` magic peek retry (R:448) | CODE |
| C6 | (changed 2026-10-16: the default is a white tint, `recolor` 30 %; a `focus_style_id` still goes over it; awaiting Olli's test) Focus style uses an outline (CT:116-120): highlights must be colours only | CODE |

## D. Concurrency risks

| # | Issue | Status |
|---|---|---|
| D1 | (changed 2026-10-16: one waiter slot per task (`alloc_waiters_`), all woken; awaiting Olli's test) `alloc_waiter_` is one slot: two requesters (playback, draw, spice) overwrite each other; one waits the full 2 s | CODE |
| D2 | Reclaim waits (2 s) are shorter than an HA picture fetch (15 s); a video start during one can fail with BUFFER_ALLOCATION_FAILED | CODE |
| D3 | (changed 2026-10-16: `acquire_session_buffers_`/`output_alloc_` take pieces without `media_lock_` and set them under it; awaiting Olli's test) `media_lock_` held during the 2 s waits (up to 3 allocations = 6 s) | CODE |
| D4 | `dev_lock_` held across blocking socket sends while the main loop waits portMAX on it (U:592-609, 1190-1205 vs 307, 331) | CODE |
| D5 | `stream_play_audio_` reachable from net task and spice task without a lock | PLAUSIBLE. Analysed 2026-10-16: not concurrent as things are: `desktop_take_panel_()` waits until a stream sender has let go before the session's channels open, and during a desktop session only svp_relay's caster gets in (stream_net_task_), which sends pictures, no sound (relay `desktop/caster.py`); the SPICE sound comes on the playback channel only. The exclusion rests on the relay's behaviour, not on a lock. Not changed |
| D6 | (changed 2026-10-16: `audio_enabled_` and `loop_` are `std::atomic<bool>`; `repeat_` (enum, main loop + controls) and `pic_read_` left) Plain fields shared across tasks without atomics: `audio_enabled_`, `loop_`, `repeat_`, `pic_read_` fields | CODE |
| D7 | `start_req_` not reset when the playback ends early: loop may still prerender, pause LVGL, fire on_started | CODE (2026-10-16): the playback task sets 1 and waits for 2 while `playing()`, at most 2 s; a stop in that window, or a main loop blocked over 2 s (A2), leaves 1, and the next loop pass prerenders, pauses LVGL (DSI direct) and fires on_started for a session already stopped; the DSI block below gives LVGL back once the task is gone. Only `start_playback_` resets it. Not changed |
| D8 | spice/net tasks prio 4 without affinity can preempt playback and loopTask (prio 1) on core 1 | CODE |
| D11 | (changed 2026-10-16 per Olli; awaiting Olli's test) The playlist decides per item whether the output mode changes (display: LVGL paused, canvas: LVGL running) and switches only then, at the item's start handshake. Before, a canvas item after a display item kept LVGL paused (video not seen) | CODE |

## E. Functional gaps / inconsistencies

| # | Issue | Status |
|---|---|---|
| E1 | `audio_session_` is only reset by `start_playback_` (S:667): stays true after a track/queue ends | CODE. Analysed 2026-10-16, not changed: between tracks it must stay true (`audio_advance_` keeps the file speaker open). After the queue's end (state STOPPED, `audio_current_` false) the stale flag only makes `stop()` set `audio_halted_`, `spice_sound_take_()` / a relay AVI call `end_session_()` on a stopped player (log "Audio file stopped" without one playing), `release_media_`/`picture_session_busy_` skip checks that a stopped player passes anyway. No functional fault found; a reset would belong at the queue's end in `audio_advance_` after the speaker finish, untested, so left |
| E2 | Audio duration/seek is dead: `duration_frames_` always 0 for audio files (S:3118); audio bar time/seek never shown | CODE |
| E3 | A local video start sets `audio_remote_ = NONE` without stopping the media player's HA track | CODE |
| E4 | Media and desktop don't fully exclude: pictures/cover can retake buffers during a desktop session (state STOPPED) | CODE |
| E5 | Stream PCM dropped whenever `state_` isn't STOPPED, also when an audio file plays on another device | CODE |
| E6 | No on_playback_started/finished for audio-file sessions | CODE |
| E7 | Status JSON doesn't report a local video/playlist to the relay ("state" is the media player's) | CODE |
| E8 | 'V' `still` is never 0 although the relay documents 0 = canvas/no stills; dashboard sender ignores 'V' changes (no `on_picture`) | CODE |
| E9 | AVI sender sends no heartbeats: a live source stalling > 30 s gets dropped | CODE |
| E10 | Panel always requests `/channels?refresh=1` (forces a full re-fetch) | CODE |
| E11 | Device YAML `target_fps: 30` vs the measured 25 fps budget (perf-notes) | CODE |
| E12 | Folder listings run synchronously on the main loop (`load_folder_playlist_` S:610, `list_directory_` P:300) | CODE |

## F. Code hygiene

- Stale comments: S:193-195, S:1378 (PPA), S:1432, S:2006, S:2564, S:2596; H:862, H:1182
  (cache buffer "internal"), H:1338, H:1050-1052, H:2006; `__init__.py`:449 (cover "kept").
  2026-10-16 corrected (checked against the code): setup canvas buffers, the `allocate_buffers_`
  call and its "allocated once in setup()", PPA rotate, "buffers only freed in destructor", PCM
  decoder "null", cache buffer "internal", cover "kept". Left: the other H refs (not located).
- Duplicates: canvas/direct release code (`release_media_` vs `handle_output_release_`), dirty-rect
  copy (R:1139 vs SP:2090), `decoded_size` (C:24) vs `jpeg_decoded_size` (P:837), bar focus logic.
- Actions registered unconditionally in `__init__.py` while their C++ exists only under `SVP_*`.

## G. Buffer plan backlog (proposal; each step needs Olli's go, one commit each)

1. Ring sized by need (bitrate x (jitter + BT delay) for streams, read-ahead for SD) instead of "the
   rest"; audio rings sized by `audio_latency` needs, not by the ring.
2. Pictures: one decoded page (the shown one), neighbours undecoded, decode buffer only on demand.
3. Cover: buffer only for the visible view; drop `cover_file_` after decode.
4. Audio: one ring by mode (input for compressed, none or small for PCM), no idle ring.
5. DSI: decode straight into the panel framebuffer if mipi_dsi permits (verify in source first).
6. Controls snapshots: reduce to what compositing needs.
7. Buffer manager: one owner table (who holds what, by mode), handover by mode switch only, no
   all-or-nothing `release_media_`; fix B11, D1-D3 there.
