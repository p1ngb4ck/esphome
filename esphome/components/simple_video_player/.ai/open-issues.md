# svp open issues (2026-10-15)

Status: **REPORTED** = Olli observed it on the device · **CODE** = confirmed by reading the code ·
**PLAUSIBLE** = likely from the code, not proven. Nothing here is fixed unless Olli ordered it.
Remove an entry only when Olli confirms the fix on the device.

## A. Bug reports from Olli

| # | Report | Notes |
|---|---|---|
| A1 | **Crash: opening a picture from an HA source while audio from an HA source plays.** Works: local audio + pictures from any source; pictures first, then audio. REPORTED | Unanalysed. Difference to local audio: the HA track brings cover art (pool buffers, channel task /cover fetch, picture-task decode) and the media player's HTTP stream. Check pool state and the cover/picture interplay on the channel and picture tasks first |
| A2 | **HA media list loading blocks the main loop**; must be asynchronous. REPORTED | The fetch is on the channel task (CL:340-391); the blocking part is on the main loop (`ha_loop_` and what its callback builds, up to 500 items). Locate exactly before changing |
| A3 | (changed: number "Slideshow picture duration" 2-60 s, on the Pictures controls, the full-screen picture bar (slide_time) and Settings > Media folders; awaiting Olli's test) "Diashow-Bilddauer" as a setting (UI + HA). Open question | Firmware has `set_slideshow_interval`; YAML only the `ui_slideshow_seconds` substitution. Olli's original list must be resent |
| A4 | SPICE folder sharing (WebDAV port channel); decide on the VNC file-transfer relay code (rfb_files.py) | TODO after remote tests |
| A5 | Cover art of local audio files via an art-db | Own topic |
| A7 | **HA audio in the queue starts with a long delay**; the same media started from HA the usual way on the device starts at once. Olli's proposal: the queue only remembers the track and triggers each one through the usual HA/ESPHome way, no own path. REPORTED | Design change; scope with Olli first |
| A8 | (changed: lead max fixed at 1600 ms instead of ring x 100 ms/MiB ~740 ms; YAML applies the BT delay only on the Bluetooth output; awaiting Olli's test. The lead is read once per session start: output switch during playback is a later special feature, after everything more important works (Olli)) **BT audio delay (`audio_latency`) is wrong**: a) doesn't work correctly, b) makes USB speakers worse, where it must not apply at all. REPORTED | Device YAML lambda uses `bt_out` delay; C++ clamps to `max_audio_latency_ms_` (S:680). Scope with Olli first |
| A9 | **Rename the svp components** (svp and its sub-projects, now far more than the video player). TODO, deliberately later: together with the review/refactor phase once the functionality is there (Olli) | Not before then |
| A10 | (changed: keyboard layouts per keyboard (YAML `keyboards:`) and per device (USB `usb:vid:pid[:serial]`, BT `bt:MAC`), from Settings > Devices > Keyboards and HA (`svp_keyboard_layout`, sensor "Keyboard layouts"); layout folder settable (YAML `directory:`, Settings > Media folders, text "Keyboard layout folder"); "New folder" in the folder chooser; several downloads at once; relay layout sources as panel options, install only from the panel (relay 0.9.41); awaiting Olli's test) Crash: OSK opened from search, then the Keyboards test field used (dangling `lv_keyboard` ta). REPORTED | Open: bundled 745 layouts in the relay zip keep or drop; `storage_options_fill` vs. an open list (freed options); SPICE guest layout (per connection or global) |
| A6 | TV regression 2026-10-15 fixed by 6a5877bba8 (cover on demand + reclaim before a video's buffers). Which of the two fixed it is not proven; likely the reclaim (no holes for the ring) | Keep in mind for B3 |

## B. Rule violations (memory / buffers)

| # | Issue | Status |
|---|---|---|
| B1 | **The ring takes the whole rest of the pool** (S:2128-2146), and audio latency + audio rings derive from it. Raising the pool only grows the ring; nothing sized by need | CODE |
| B2 | (changed in the commit after 775e5ff: 2 pages, neighbours undecoded, decode buffer on demand; awaiting Olli's test) **Pictures pre-decode**: 3 decoded pages (P:519) + a kept native decode buffer (grows, P:563-577) + 3 compressed files; the same picture compressed and decoded at once | CODE |
| B3 | Pictures and cover are not in the pool plan; they live off whatever the ring leaves or by freeing all media buffers (`release_media_` is all-or-nothing, P:501, C:61). Fragmentation possible when pieces stay inside the free area | CODE / PLAUSIBLE |
| B4 | Cover: all 3 views are decoded and held while cover art shows, though only the visible one is needed; `cover_file_` (512 KiB) kept after decode | CODE |
| B5 | DSI direct: decoded frame in `decode_target_` (2.05 MB) is copied into the panel framebuffer every frame (S:1402): a full frame twice. Decoding straight into the framebuffer: unverified whether mipi_dsi allows it | CODE / unverified |
| B6 | One of the two audio rings is always idle: PCM uses only the decoded ring, MP3/FLAC only the input ring; both held for good (S:2577, 2638). Decoded PCM ring conflicts with "undecoded only" | CODE |
| B7 | After a session the canvas RGB888 pair and `decode_target_` stay allocated (decoded, not shown) until a reclaim | CODE |
| B8 | `index_buf_` is allocated for network AVI and raw MJPEG, which use no index | CODE |
| B9 | Controls: 3 × bar snapshot reserved (CT:331-342); check need | CODE |
| B10 | Allocations past the pool: `channel_http_` response buffer (PSRAM realloc up to 256 KiB per request, CL:578); PNG/BMP decode via RAMAllocator; `PsramPool::pieces_` grows on the internal heap past 32 pieces | CODE |
| B11 | `wait_alloc_answer_` expiry: after the 2 s timeout `pool_reclaim_req_`/`output_release_req_` stay set; the main loop frees later for nobody (H:2110-2117) | CODE |

## C. Rule violations (speakers, LVGL, hot path)

| # | Issue | Status |
|---|---|---|
| C1 | `file_speaker_` (audio queue resampler) is still stopped/finished/paused by the player (S:3194, 3230, 2942, 3086, 2861). Speaker rule. Open question to Olli: a resampler takes its input format only at start | CODE |
| C2 | LVGL touched while paused: `prerender_controls_` for the next playlist item while LVGL is still paused (S:294 vs 327); `slide_thumbnails_` auto-hide before the held check (P:141); `lv_obj_is_hidden` on the picture bar every loop (CT:904); pictures guarded only by `state_` (udisp/desktop sessions pause LVGL with state STOPPED); `set_options` (CL:207), `lv_image_set_src` in `handle_output_release_` (S:2311) / `release_media_` (S:2364), `release_picture_buffers_` (P:586) without a pause guard | CODE |
| C3 | Two different "paused" guards in use: `lvgl_component_->is_paused()` vs `dsi_lvgl_paused_` | CODE |
| C4 | Logs in hot paths: `ESP_LOGW "Dropped %zu bytes"` per audio chunk unlimited (S:2628); audio decode error repeats (S:2823); `ESP_LOGE` pool no-room per stream frame + 2 s wait per frame when the output can't be had (S:2165, R:1037/1049); usbredir skip per packet (U:678) | CODE |
| C5 | Polling: cover/picture completion flags polled every loop; `vTaskDelay(20)` loop in `desktop_take_panel_` (SP:1496); `vTaskDelay(1)` magic peek retry (R:448) | CODE |
| C6 | Focus style uses an outline (CT:116-120): highlights must be colours only | CODE |

## D. Concurrency risks

| # | Issue | Status |
|---|---|---|
| D1 | `alloc_waiter_` is one slot: two requesters (playback, draw, spice) overwrite each other; one waits the full 2 s | CODE |
| D2 | Reclaim waits (2 s) are shorter than an HA picture fetch (15 s); a video start during one can fail with BUFFER_ALLOCATION_FAILED | CODE |
| D3 | `media_lock_` held during the 2 s waits (up to 3 allocations = 6 s) | CODE |
| D4 | `dev_lock_` held across blocking socket sends while the main loop waits portMAX on it (U:592-609, 1190-1205 vs 307, 331) | CODE |
| D5 | `stream_play_audio_` reachable from net task and spice task without a lock | PLAUSIBLE |
| D6 | Plain fields shared across tasks without atomics: `audio_enabled_`, `loop_`, `repeat_`, `pic_read_` fields | CODE |
| D7 | `start_req_` not reset when the playback ends early: loop may still prerender, pause LVGL, fire on_started | PLAUSIBLE |
| D8 | spice/net tasks prio 4 without affinity can preempt playback and loopTask (prio 1) on core 1 | CODE |

## E. Functional gaps / inconsistencies

| # | Issue | Status |
|---|---|---|
| E1 | `audio_session_` is only reset by `start_playback_` (S:667): stays true after a track/queue ends | CODE |
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
