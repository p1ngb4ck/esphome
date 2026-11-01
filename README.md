# esphome-emulator

ESPHome `dev` plus the game emulator app of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for emulator and display. An app of its own (one repo per app); branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/emulator` | emulator: game emulator app (NES, Game Boy, Master System, PC Engine, Lynx, MSX, Game & Watch, SNES, Mega Drive): one non-blocking PPA pass per frame into the video output's rows, a busy PPA skips the frame, the measured pass time steps the scale down (kept per system), systems gated by the screen size at config time (`min_scale`), keys through LVGL's key tap, save files / states through the storage worker, players, link | `pr/display-video-output-rows` ([esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)), `pr/esp32-idf-component-override-path` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) | P4 (PPA, scanout) |
| `pr/emulator-close` | emulator: close gesture - the close key held (key_bindings Back / Home) or two fingers held (`touchscreen_id`) ends the game, `closed_home()` | `pr/emulator` | P4 |
| `pr/emulator-close-action` | emulator: the close gesture a configured key binding (`close_action`, a key_bindings action, default `back`) | `pr/emulator-close` | P4 |

The cores (`emulator/emu`) are an IDF component; each core's directory carries its licence file or README.
