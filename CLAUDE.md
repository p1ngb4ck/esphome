# Hard Rules for This Repo

This file is loaded automatically at the start of every session in this project — no action
needed to "turn it on". It exists because rules that only live in a chat transcript get
re-litigated and re-broken every time context resets. These do not get re-litigated.

Also read [`AGENTS.md`](AGENTS.md) (ESPHome's general contributor guide — build system, coding
conventions, the existing "Verify Before Code Generation" rule about struct fields/method names).
The rules below are stricter and more specific, and apply on top of it.

## 1. Verify BEHAVIOR, not just signatures — every time, no exceptions

AGENTS.md already bans guessing field/method names. This extends that to runtime *behavior* and
*semantics* of anything from FreeRTOS, ESP-IDF, LVGL, or an upstream ESPHome component you did not
just write — a wrong behavioral assumption compiles fine and fails silently at runtime, which is
strictly worse than a compile error and is what caused most real bugs this project has hit.

**Before stating how something behaves, fetch and quote the actual source. Every time.**

- FreeRTOS/ESP-IDF semantics (does X block? does X yield to a lower-priority task? what does flag Y
  actually do?) → `WebFetch` the real source from `github.com/espressif/esp-idf` (or
  `raw.githubusercontent.com/espressif/esp-idf/<tag>/...`), not memory. Example from this project's
  own history: `taskYIELD()` does **not** let a lower-priority task run — verified against
  `tasks.c`'s `taskSELECT_HIGHEST_PRIORITY_TASK()` — only `vTaskDelay()` (a real Blocked-state
  transition) does. This was gotten wrong from trained "general knowledge" twice before it was
  actually checked.
- LVGL internals (buffer ownership, zeroing, cache behavior, widget lifecycle) →
  `WebFetch` `github.com/lvgl/lvgl` at the pinned version this repo uses. Example: `lv_malloc_core()`
  on ESP32 is a plain `heap_caps_malloc()` — **not zeroed** — verified against the real
  `lvgl_esphome.cpp`.
- An existing ESPHome component's behavior (e.g. `ring_buffer::RingBuffer`, `RAMAllocator`,
  `storage::StorageWorker`) → **Read the actual file in this repo first** (it's already local,
  no fetch needed) — `esphome/components/<name>/...`, `esphome/core/helpers.h`. This repo's own
  copy is more authoritative than upstream for how it will actually compile and run.
- ESPHome's own codegen/build-order behavior (what runs before what) → read `esphome/writer.py`
  and the actual `to_code()` of the component in question, don't assume from convention.

If you have not just done one of the above for a specific claim, say "I haven't verified this yet"
and go do it — do not present it as fact, and do not silently substitute local-file reading when
asked for web research (say which one you're doing).

## 2. Prefer an existing upstream component over hand-rolling one

Before writing a custom synchronization primitive, buffer, or data structure, check whether
`esphome/components/` already has one. This project reinvented a ring buffer (slot array + two
raw counting semaphores) when `esphome::ring_buffer::RingBuffer` already existed and was already in
use elsewhere in the same file. Grep `esphome/components/` before building anything that feels like
infrastructure.

## 3. This is an ESP32-P4 MCU: 512KB internal SRAM, PSRAM for everything real

- Any buffer that can be more than a few KB **must** be PSRAM, and must **hard-fail** (return
  false / mark_failed(), log why) if PSRAM can't provide it — never let an allocator silently fall
  back to internal SRAM for something that size. `RAMAllocator`'s `NONE`/`EXTERNAL_FIRST` modes
  (and anything built on them, like `ring_buffer::RingBuffer::create()`) enable internal RAM as a
  fallback; verify capacity with `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`
  *before* calling into them if the size could plausibly need SRAM to fail instead of silently
  succeeding somewhere unsafe.
- No `ESP_LOG*` calls anywhere in a hot/priority-10 execution path. Logging itself is expensive
  enough on this MCU to be a real cause of missed deadlines, not just noise.
- No blocking waits of any fixed duration in a hot path, however short. `xSemaphoreTake(h, 0)` is a
  real non-blocking try; any nonzero `pdMS_TO_TICKS(N)` is a real wait and eats directly into the
  ~40ms/frame total budget shared by decode + audio + storage prefetch.
- The video panel is one fixed resolution, compiled once from one YAML per device. Never write
  runtime resize/reflow logic for it — there is no case where it changes.
- Never implement frame-dropping/catch-up-on-lag logic for video playback. This MCU cannot
  structurally catch up once behind; the only correct strategy is fire-and-forget at the paced,
  correct cadence and letting decode take however long it takes, optimizing everything upstream of
  presentation instead.

## 4. When stuck, diff against history instead of re-deriving from scratch

`E:/esphome-opencode/testing_dev_old_ref` is a permanent git worktree at an old, partially-working
commit of this component, kept specifically for this. When behavior regresses or a mechanism is
unclear, compare against it before guessing.
