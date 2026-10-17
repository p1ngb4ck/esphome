# simple_video_player (svp) — start here, every session

Read in this order before touching anything svp (or svp_relay):

0. [`session-todo.md`](session-todo.md) when it exists: Olli's standing order for the current session, binding.

1. This file: hardware/environment facts, goals, hard rules, change procedure.
2. [`architecture.md`](architecture.md): how the code is built today (tasks, locks, sessions, buffers).
3. [`open-issues.md`](open-issues.md): known violations, bug reports, the optimisation backlog.
4. [`protocol.md`](protocol.md): only when the panel <-> svp_relay link is involved.
5. [`perf-notes.md`](perf-notes.md): measured history (2026-09/10). Partly outdated, see the note at the end.

Also binding: the repo's [`CLAUDE.md`](../../../../CLAUDE.md) and `AGENTS.md`.

Everything in these files was read from the code (file:line) unless marked *unverified*. When the
code and these files disagree, the code wins: fix the file in the same commit, never the other way
round without Olli's go.

---

## 1. Hardware and environment (facts)

| Item | Fact |
|---|---|
| MCU | ESP32-P4 (engineering sample, silicon < v3: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`), 2 HP cores, 360 MHz in the device YAML |
| RAM | 512 KB internal SRAM (scarce), 32 MB PSRAM. svp gets one PSRAM pool (`psram_pool_size`, device: 15 MB). Idle free PSRAM on the device: ~5.5 MB (Olli, 2026-10-15) |
| IDF | Must build with ESP-IDF 5.5.5 (ESPHome's) and 6.1 (Olli's device: 6.1, `release/v6.1`) |
| Display | MIPI-DSI panel 800x1280 native, LVGL screen 1280x800 (rotated). DSI-direct video: decoded frame copied into the panel framebuffer; LVGL paused meanwhile |
| Decoders | HW JPEG decoder (the only video path: MJPEG), shared by video, stream stills, pictures, cover (driver `codec_mutex` serialises). No H.264. PNG/BMP in software |
| Audio out | Speakers: I2S (es8311), USB audio, Bluetooth A2DP. `audio_router` picks the output; mixers/resamplers in YAML. Speaker media player (HA audio) separate |
| Network | Wi-Fi via esp32_hosted (ESP32-C6 over SDIO, `use_psram: true`) and Ethernet |
| Storage | SD (sd_storage), USB mass storage, NFS, via the storage subsystem (`storage` component) |
| Other users of PSRAM/RAM | LVGL, mipi_dsi framebuffer(s), storage `psram_transfer_buffer_size: 2MB`, speaker media player `buffer_size: 1000000`, BT/A2DP, micro_wake_word, voice_assistant, USB host, task stacks |
| Frame budget | 25 fps = 40 ms per frame for decode + present + audio + storage (perf-notes). Device YAML has `target_fps: 30`; the stream fps is a relay option, set at runtime |
| Relay | svp_relay: Home Assistant custom component (HACS), not in this repo; delivered as a zip (current 0.9.45). Optional svp-relay worker on another machine (out of scope for now) |
| Build/test | Olli compiles and tests. Never compile, run config checks or tests yourself |

## 2. Goals (Olli's requirements)

- A media player as close as possible to the user experience of players on real CPUs with an OS:
  the MCU can do everything, but only a few things at the same time. The art is RAM size and
  storage I/O, not CPU (video: HW JPEG; FLAC: ~7-8 % CPU).
- Media kinds: pictures, audio, video, live/TV video streams. Sources: local (SD/USB/NFS) or Home
  Assistant (incl. everything svp_relay produces).
- svp_relay transcodes on demand, as fast as possible: not realtime, but lowest possible latency.
- Every device feature is operable remotely (Home Assistant), never only on the screen.
- General-purpose component: Olli's hardware only limits what he can test.
- Monolithic svp for now (later split into components), but structured inside: one mechanism per
  job, no duplicated handling, no feature working past the shared infrastructure.

## 3. Hard rules (invariants) — never broken, never re-litigated

### Memory
- **All svp buffers come from the PSRAM pool, through the buffer manager.** No svp function allocates
  past it (no own `heap_caps_*`, no fixed reservations for one feature). Exceptions only with
  Olli's go and listed in architecture.md (today: small internal buffers for SPICE input/USB, up to
  32-64 KB, where it helps performance).
- **Buffers are taken only when needed and handed over between functions**; reallocation only when
  the function switches (video->video: none; video <-> pictures/cover/desktop: hand over), never per
  file. Media playback/dashboard and remote desktop exclude each other.
- **Data is buffered undecoded.** Decoded buffering only for video frames where needed and for what
  is on screen. No pre-decoded pictures; an extra decode buffer only on demand for larger pictures.
- **Sizes are tuned for real performance**: too much is as wrong as too little (bus/cache, other
  components starve).
- Large buffers in PSRAM must hard-fail (log why) when PSRAM can't give them; never fall back to
  internal RAM silently. Nothing goes to internal RAM that PSRAM could hold.
- No destroy/re-create of tasks, objects or buffers when reuse is possible.

### Speakers
- **Speakers are always on.** Playback only sends data to the currently selected speaker. svp never
  starts, stops, pauses, re-formats or otherwise changes a speaker; only `audio_router` starts/stops
  its output when the output is switched. Speaker buffers belong to the speaker and are not touched.
  (Open: `file_speaker_` still violates this, see open-issues.)
- The default audio path (speaker media player, HA audio to the device) stays as it is.

### Sessions
- A running desktop session, video, stream or dashboard is never interrupted by a call from outside
  (HA, relay); a call made on the panel itself (opening the dashboard, another kind of media) ends
  what runs and takes over (Olli, 2026-10-17).
- The kinds of media and the apps (dashboard, remote desktop) exclude each other; the buffer manager
  hands the buffers over on a change of kind where needed. Only these run side by side: audio and
  pictures; audio and a VNC desktop without sound, while the pool has room for both (Olli, 2026-10-17).

### LVGL / display
- Never call LVGL (any widget, even hidden, even a read) while LVGL is paused or DSI-direct runs.
- LVGL updates are event-driven, guarded by "LVGL runs" and "element active". No interval-driven
  LVGL updates; anything periodic checks the situation first and returns when in doubt.
- Controls over a playing video are pre-rendered before playback; never LVGL-drawn during it.
- LVGL 9.6 API: flags only through the dedicated setters (`lv_obj_set_hidden`, `lv_obj_set_clickable`, ...);
  never `lv_obj_add_flag` / `lv_obj_remove_flag` (deprecated, warnings), in C++ and in YAML lambdas.
- The device never resizes, rescales or reflows at runtime. Its picture size is fixed in YAML; the
  panel reports it to the relay ('V'); only the relay fits pictures to it.
- UI: design (mockup) first, Olli approves, then implement. Highlights by colour only. Touch and keys
  (remote, keyboard, gamepad) always both work, with a correct focus order.

### Hot paths (playback, decode, present, audio feed)
- No `ESP_LOG*`. No fixed-duration blocking waits (`xSemaphoreTake(h, 0)` is the only allowed take).
- No frame dropping / catch-up logic: present at the paced cadence and move on.
- Event-driven everywhere; no polling (MCU, not a server).

### Behaviour of the assistant (Olli's standing rules)
- Do exactly the asked task. No extras, no "while I'm here", no unrequested scripts/mechanisms.
- Never remove anything (code, config, features, mappings) unless Olli says so.
- No assumptions: verify behaviour in source (this repo first; IDF/LVGL upstream at the used
  version) before stating it. Say "unverified" otherwise.
- Complaints and questions are not work orders: answer and stop. Lists Olli gives are saved at once
  (open-issues.md or the session's TODO file).
- Commits: `git -c user.name=p1ngb4ck -c user.email=kleinecke.oliver@googlemail.com commit --no-verify`,
  push to `origin testing_dev`, no trailers, never amend.
- Device configs delivered: `lvgl_media.yaml`, `esp32-p4-media.yaml` only, sent to Olli as files
  (SendUserFile) every time either changes. Relay: zip, sent the same way.

## 4. Change procedure (every change, no exceptions)

1. **Read** this folder (at least README + the architecture sections the change touches).
2. **Scope** in writing before coding: what changes (files/functions), which invariants and which
   buffer-table rows it touches, what it must not change. Ask Olli when anything is open.
3. **Integrate, don't add beside**: use the existing mechanism (buffer manager, session handling,
   handshakes, controls bar code). If none fits, propose extending it; don't build a second one.
4. **Implement** only the scope.
5. **Diff review** line by line against the scope and the rules: everything not asked for goes.
6. **No own checks**: no review agents, no config checks, no compiles, no tests. Olli builds and
   tests, without exception (Olli, 2026-10-16).
7. **One topic per commit.** If a fact in these files changed, update them in the same commit.
8. Olli builds and runs the regression checklist below.

## 5. Regression checklist (Olli, after each build)

- [ ] TV / live stream via relay (DSI direct): smooth, sound, channel switch
- [ ] SD video: start, pause/resume, seek, next in playlist, end
- [ ] Video in a canvas (if configured)
- [ ] Pictures: local and HA, next/previous, slideshow, fullscreen + controls
- [ ] Audio queue: local file, HA track, next/previous, pause, end of queue; cover art (queue/tab/fullscreen)
- [ ] Pictures while audio plays (local and HA audio); video after pictures and back
- [ ] Dashboard (relay), close by key/two fingers
- [ ] Remote desktop (SPICE/VNC), USB redirection, back to media
- [ ] Speaker output switch (I2S/USB/BT) during and between playback
- [ ] Free PSRAM at idle and after each of the above (debug sensors), no growth

## 6. Note on perf-notes.md

Historic and partly superseded. Still true per code: video task prio 1 on core 1, wall-clock pacing,
no frame dropping, one compressed frame slot (`FRAME_SLOT_COUNT = 1`), the 25 fps / 40 ms budget.
Superseded: "two decoded frame buffers" (DSI direct uses one `decode_target_`; canvas output uses an
RGB888 pair), "4 MB ring" (the ring is now prebuffer + the sound's lead at 4 MiB/s), the speaker being started/stopped by
the player (removed, f0b518d). The PCM decoded-ring rationale there conflicts with the
undecoded-buffering rule: see open-issues.
