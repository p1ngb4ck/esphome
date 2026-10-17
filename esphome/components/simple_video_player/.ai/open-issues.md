# svp open issues (2026-10-16)

Line references (S:, P:, CT:, CL:, R:, U:) were taken on 2026-10-15; the code has moved since, search by name.

Status: **REPORTED** = Olli observed it on the device · **CODE** = confirmed by reading the code ·
**PLAUSIBLE** = likely from the code, not proven. Nothing here is fixed unless Olli ordered it.
Remove an entry only when Olli confirms the fix on the device.
Only what Olli names goes on this list (Olli, 2026-10-16). REPORTED issues always come first.

## A. Bug reports from Olli

| # | Report | Notes |
|---|---|---|
| A16 | **Bluetooth audio: rework as a whole** (Olli, 2026-10-17). REPORTED. Its parts: a) **TV (live stream through the relay) with BT audio is badly broken**: the most important bug fix; noted only, nothing done yet (Olli: note it, do nothing). b) **BT audio delay (`audio_latency`) does not work correctly** (was A8, REPORTED). Its second half, the delay making USB speakers worse, is fixed: it no longer applies to them (Olli, 2026-10-17). Changed: lead max from the pool room the prebuffer leaves (2026-10-17), given to the BT delay's slider and number; YAML applies the BT delay only on the Bluetooth output; awaiting Olli's test. The lead is read once per session start: output switch during playback is a later special feature, after everything more important works (Olli). c) **Reconnect initiated by the device does not work correctly**, everything else does (was A14, REPORTED 2026-10-16). d) **Stuttering start (2-3 s) and short stutter on LVGL/picture actions** (REPORTED 2026-10-17). Changed: a2dp buffer 300 ms (`buffer_duration`), from PSRAM only, filled once before the stream starts; cache write-backs in 128 KB chunks; the BT chain's task stacks internal (`bt_44k`, `bt_mixer`, `media_spk_resampling_input`, media player decoder; the announcement decoder's too, up to 5 KB more: kept (Olli, 2026-10-17)); decode and resample tasks at priority 2. Much better (Olli); a short stutter on a media type / source change, changed again; awaiting Olli's test. e) **Internal RAM drops ~100 KB at the start of BT audio** (REPORTED 2026-10-16): better after mbedTLS in PSRAM and malloc > 1 KB from PSRAM; whether it is still there is open. f) **HA audio in the queue starts with a long delay** (was A7, REPORTED); the same media started from HA the usual way on the device starts at once. Olli (2026-10-17): a normal delay is normal (the USB HDD with the media on the Pi 5 spinning up; the Pi 5 is no fast NAS); together with the long prebuffering/start on BT it became this long. Partly this BT problem, the rest is A18. Olli's earlier proposal: the queue only remembers the track and triggers each one through the usual HA/ESPHome way, no own path g) Possibly involved, not known yet (Olli, 2026-10-17): the one sound block (was B6: one block as large as the larger ring kind the configuration uses, the ring over it the kind the file needs, made anew only when the kind changes); otherwise it works | b) Device YAML lambda uses `bt_out` delay; C++ clamps to `max_audio_latency_ms_` (S:680). c) a2dp_source. Do not act alone: analyse, then scope with Olli |
| A4 | SPICE folder sharing (WebDAV port channel); decide on the VNC file-transfer relay code (rfb_files.py) | TODO after remote tests |
| A5 | Cover art of local audio files via an art-db | Own topic |
| A9 | **Rename the svp components** (svp and its sub-projects, now far more than the video player). TODO, deliberately later: together with the review/refactor phase once the functionality is there (Olli) | Not before then |
| A10 | (changed: keyboard layouts per keyboard (YAML `keyboards:`) and per device (USB `usb:vid:pid[:serial]`, BT `bt:MAC`), from Settings > Devices > Keyboards and HA (`svp_keyboard_layout`, sensor "Keyboard layouts"); layout folder settable (YAML `directory:`, Settings > Media folders, text "Keyboard layout folder"); "New folder" in the folder chooser; several downloads at once; relay layout sources as panel options, install only from the panel (relay 0.9.41); awaiting Olli's test) Keyboard layouts. The crash (OSK opened from search, then the Keyboards test field; closing the OSK) is gone (Olli, 2026-10-17) | Open: the bundled 745 layouts go out of the relay zip, they are downloaded instead (Olli, 2026-10-17; not done yet); the download still has a bug (Olli, not described yet); `storage_options_fill` vs. an open list (freed options): changed 2026-10-17 (closed, filled, opened again, as `browser_storage_fill`), not seen on the device |
| A13 | Header icons: rework; already very good, not yet perfect (Olli, 2026-10-16). TODO | Details from Olli; design first |
| A15 | On-screen keyboard settings, most important: "show the on-screen keyboard even if a physical keyboard is connected" (default no); in LVGL settings and reachable from HA. TODO 2026-10-16 | Scope with Olli first |
| A19 | **HA audio in the queue: a track's end is not seen** (Olli, 2026-10-17): the first track stays on playing although it played to its end, the next one does not start; nothing about it in the log. REPORTED | Diagnostics in the audio pipeline (DEBUG: reader/decoder done, stopped, no progress for 5 s; 103cdb59eb), to come out after; waiting for Olli's log |
| A18 | **svp_relay on the Pi 5: stability and performance** (Olli, 2026-10-17). TODO | Scope with Olli first |

## B. Rule violations (memory / buffers)

| # | Issue | Status |
|---|---|---|
| B10 | `channel_http_` response buffer outside the pool: PSRAM heap, `heap_caps_realloc` up to 256 KiB per request (CL). Checked 2026-10-17 | CODE |
| B11 | `pool_reclaim_req_` stays set after the 2 s `wait_alloc_answer_` timeout: the main loop frees the pictures/cover buffers later for nobody (the output request is taken back already). Checked 2026-10-17 | TO TEST: owner table, a request not answered in 2 s is taken back by the requester (2026-10-17) |

## C. Rule violations (speakers, LVGL, hot path)

| # | Issue | Status |
|---|---|---|
| C1 | `file_speaker_` (audio queue resampler) is still stopped/finished/paused by the player (S:3194, 3230, 2942, 3086, 2861). Speaker rule. Open question to Olli: a resampler takes its input format only at start | CODE |
| C2 | LVGL touched while paused, still to check in the code (2026-10-17): `prerender_controls_` for the next playlist item, the `pool_reclaim_req_` block in `pictures_loop_` before the held check (`picture_decoded_()` -> `show_page_`), `lv_image_set_src` in `handle_output_release_` / `release_media_`, `release_picture_buffers_`, the picture bar's `lv_obj_is_hidden` each loop | CODE (unchecked since) |

## D. Concurrency risks

| # | Issue | Status |
|---|---|---|
| D2 | Reclaim waits (2 s) are shorter than an HA picture fetch (15 s); a video start during one can fail with BUFFER_ALLOCATION_FAILED | TO TEST: the fetch is dropped (not writing yet) or stopped after its block, the video starts (Olli, 2026-10-17) |
| D4 | `dev_lock_` held across blocking socket sends while the main loop waits portMAX on it (U:592-609, 1190-1205 vs 307, 331) | CODE |
| D6 | Plain fields shared across tasks without atomics: `repeat_` (enum, main loop + controls), `pic_read_` fields | CODE |
| D7 | `start_req_` not reset when the playback ends early: loop may still prerender, pause LVGL, fire on_started | CODE (2026-10-16): the playback task sets 1 and waits for 2 while `playing()`, at most 2 s; a stop in that window, or a main loop blocked over 2 s (A2), leaves 1, and the next loop pass prerenders, pauses LVGL (DSI direct) and fires on_started for a session already stopped; the DSI block below gives LVGL back once the task is gone. Only `start_playback_` resets it. Not changed |
| D8 | spice/net tasks prio 4 without affinity can preempt playback and loopTask (prio 1) on core 1 | CODE |

## E. Functional gaps / inconsistencies

| # | Issue | Status |
|---|---|---|
| E2 | Audio files: no duration/seek (`duration_frames_` is set only from a video's info: the audio bar's time and seek never show) and no on_playback_started/finished for their sessions (was E6). One piece of work (Olli, 2026-10-17) | CODE |
| E7 | Status JSON doesn't report a local video/playlist to the relay ("state" is the media player's) | CODE. A real gap (Olli, 2026-10-17) |

## F. Code hygiene

- Stale comments: S:193-195, S:1378 (PPA), S:1432, S:2006, S:2564, S:2596; H:862, H:1182
  (cache buffer "internal"), H:1338, H:1050-1052, H:2006; `__init__.py`:449 (cover "kept").
  2026-10-16 corrected (checked against the code): setup canvas buffers, the `allocate_buffers_`
  call and its "allocated once in setup()", PPA rotate, "buffers only freed in destructor", PCM
  decoder "null", cache buffer "internal", cover "kept". Left: the other H refs (not located).
- Duplicates: canvas/direct release code (`release_media_` vs `handle_output_release_`), dirty-rect
  copy (R:1139 vs SP:2090), `decoded_size` (C:24) vs `jpeg_decoded_size` (P:837), bar focus logic.
- Actions registered unconditionally in `__init__.py` while their C++ exists only under `SVP_*`.

## G. Buffer plan backlog (each step needs Olli's go, one commit each)

1. Buffer manager: one owner table, handover only when room is missing (Olli: always the first
   rule), no all-or-nothing `release_media_`; fix B11 and D2 there. TO TEST: implemented 2026-10-17
   (architecture.md section 9). The desktop no longer gives back pictures and video at its start
   when its buffers fit.
