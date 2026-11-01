# esphome-emulator

`emulator`: a game emulator app for the ESP32-P4 - NES, Game Boy / Color, Master System / Game Gear, PC Engine,
Lynx, MSX, Game & Watch, SNES and Mega Drive. Games are files on a storage; the picture is scaled by the P4's PPA in
one pass straight into the display's frame buffer, the sound goes to an ESPHome speaker, and any HID input (USB or
Bluetooth gamepads, keyboards) or the touchscreen plays. Save states, several players and a network link between two
devices are built in.

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-emulator@main
    components: [emulator, display, lvgl, hid, storage]

emulator:
  lvgl_id: lvgl_main
  speaker: main_speaker
  hid_ids: [gamepad]
  systems:
    nes: /sdcard/games/nes
    gb: /sdcard/games/gb
    md: /sdcard/games/md

button:
  - platform: template
    name: Start game
    on_press:
      - emulator.start:
          path: /sdcard/games/nes/game.nes
```

Needs a MIPI DSI display with scanout (esphome-mipi-display) and LVGL's video output (esphome-esp-video). Only
the systems listed are built in.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option - systems and their folders, scaling, keys and menu keys,
players, the network link, save states, the close gesture, all actions and triggers.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/emulator` | `emulator`: the game emulator app - one non-blocking PPA pass per frame into the video output's rows, the scale stepped down by the measured pass time (kept per system), systems gated by the screen size at build time (`min_scale`), keys through LVGL's key bindings, save files / states through the storage worker, players, link | `pr/display-video-output-rows` ([esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)), `pr/esp32-idf-component-override-path` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) | P4 |
| `pr/emulator-close` | the close gesture - the close key held or two fingers held (`touchscreen_id`) ends the game | `pr/emulator` | P4 |
| `pr/emulator-close-action` | the close gesture a configured key binding (`close_action`, default `home`) | `pr/emulator-close` | P4 |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

The emulator cores come from [Retro-Go](https://github.com/ducalex/retro-go) by Alex Duchesne and its contributors,
and the cores' own authors: nofrendo (Matthew Conte), gnuboy (Laguna, Gilgamesh), SMS Plus (Charles MacDonald),
PCE-GO / HuExpress, Handy (K. Wilkins), fMSX (Marat Fayzullin), Snes9x (Gary Henderson, Jerremy Koot and the Snes9x
team), Gwenesis and LCD-Game-Emulator (bzhxx). Thank you all - decades of emulation work made this possible. Each
core keeps its licence file in `esphome/components/emulator/emu`. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
