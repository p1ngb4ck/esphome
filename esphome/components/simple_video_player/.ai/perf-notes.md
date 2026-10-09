# simple_video_player — rendering-time & performance hard facts

Extracted from the component's own git history (commit hashes in parens). These are measured/root-caused
facts from the playback bring-up, kept so they are not re-discovered or re-broken.

## The "functionally correct, performance not good enough" state
- 2026-09-07, tip at `5c8a322ce` (on top of `fc8f9b827` "single RGB888 output buffer, old-dev model"
  and `d4e658a11` DSI path). At that point everything worked — audio decoupled, wall-clock pacing,
  no tearing, EOF/underrun handled — but throughput still wasn't enough.
- The PORTALL reader/decoder split (`859831d`, 2026-10-08) was the next perf attempt; it introduced the
  4-slot compressed pre-buffer that later over-allocated PSRAM.
- Last known-good before the 09-07 perf series: `a7b5617eb` (revert target of `9cff58090`).

## Frame budget / pacing
- HARD TARGET: 25 fps = 40 ms/frame. The device cannot sustain 30 fps. The 40 ms is the budget for
  EVERYTHING per frame: HW-JPEG video decode + PPA rotate & render + audio decode (parallel on
  Core 0) + pacing + video-file read/fetch. Use target_fps: 25, not 30.
- Pacing is a wall-clock compare spin (esp_timer_get_time vs target). NO vTaskDelay on the play path,
  NO frame-dropping, NO catch-up snap: when behind, present now and move on (`4dc7005aa`).

## Cores / priority (DMA2D serialization — esp-idf#18999)
- Video/decode task: prio 1 (== loopTask), pinned Core 1 (was prio 10, `a22b4bc01`). Equal priority =
  tick round-robin keeps LVGL alive without a per-frame yield. Core 1 keeps HW-JPEG decode (DMA2D) and
  PPA rotate (DMA2D) from ever running concurrently (that concurrency hangs DMA2D, esp-idf#18999).
- Audio feed task: prio 1, Core 0, never sleeps.

## Buffers / PSRAM
- Decoded frame buffer: RGB888, ALIGN_UP(1280,16)·ALIGN_UP(800,16)·3 ≈ 3 MB each. RGB888 not RGB565 —
  the P4 HW JPEG decoder's RGB565 output is buggy on some silicon revs.
- Correct buffer set (user spec): ONE compressed video-file buffer (absorbs storage gaps) + TWO decoded
  frame buffers (render double-buffer; decode the next frame into the free one). NOTHING pre-decoded.
- Read-ahead ring: 4 MB PSRAM (grew 512 KB → 4 MB, `f75ba8b6a`). 512 KB couldn't bridge ~800 ms of
  blocking main-loop setup (lvgl/audio init) during which on_fill_done_ can't run (~300 ms just
  lvgl/logger, `b2d32020f`). Precache the ring full before switching to streaming reads.
- Per-play memset only the region the first decode won't cover, not the whole ~3 MB (`5c8a322ce`).

## Hot-path costs measured and removed
- Blocking read() (20 ms steps, 5 s cap) parked the video task ~50 % of wall time under load → one
  non-blocking ring drain instead (`c501c06e6`); underrun deliberately not handled there.
- The pace spin must NOT run storage_worker->update()/wdt_reset every iteration (millions/s) — it
  burns the CPU + PSRAM/bus bandwidth the panel needs. One update() per frame (top of loop) is enough
  (`6bbef8e9b`). But update() MUST be pumped from the play loop: the Core-1 task never yields, so
  otherwise read_chunk completions never fire and the ring never refills (`54c9596e1`).
- Per-frame esp_cache_msync (2 MB M2C) removed (`5a98911b8`). Per-frame lv_canvas_set_draw_buf dropped
  for a bare data-pointer swap + invalidate (`fec2d2181`).

## Audio
- Underrun root cause: a whole video-frame of PCM pushed into speaker->play() with ticks_to_wait=0
  every ~40 ms → the speaker took ~1/4 and dropped the rest, then starved. Fix: PCM always into
  audio_decoded_ring_buffer_, the svp_audio task drains it to the speaker at a steady rate with a
  blocking retry (`9163001c7`). Formats are locked end-to-end (config == file == speaker) → straight
  byte copy, no channel conversion.

## PSRAM budget caution
- All of the above shares PSRAM with the storage TransferBuffer arena (default 25 % of PSRAM) and any
  other component (e.g. a separate speaker_media_player). Over-allocating here (e.g. 4 compressed
  slots, or a second large buffer) can starve another component's allocation with ESP_ERR_NO_MEM.
