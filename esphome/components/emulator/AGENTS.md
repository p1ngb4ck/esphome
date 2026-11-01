# emulator: guide for AI agents

The game emulator app (ESP32-P4).

- `emu/`: the cores as an IDF component (`add_idf_component(name="emu", override_path=...)`), each with its own
  licence: nofrendo, gnuboy, smsplus, pce-go, handy, fmsx, snes9x, gwenesis, gw-emulator (Retro-Go ports).
  `emu/ports/*_port.*` adapt each core to this component; `emu/shim/` replaces Retro-Go's system layer
  (`rg_system.h`, memory, forced settings). Keep core sources as close to upstream as possible; changes go into the
  ports and the shim. Only configured systems are compiled (`CONFIG_EMU_<SYSTEM>`).
- `emulator.*`: the app - game start / stop / pause, the frame loop on its own task, one non-blocking PPA pass per
  frame into the display's rows (`VideoOutput::open_rows` / `rows_begin` / `rows_end`); a busy PPA skips that frame's
  pass, and the measured pass time steps the scale down (kept per system in the preferences). Sound goes to the
  speaker in the core's rate.
- `emu_cores.cpp`: the system table; `emu_link.cpp`: the network link (one TCP listen + one connection socket,
  declared with `socket.consume_sockets()`).
- Input: HID inputs and LVGL key bindings are read on the main loop and handed to the frame task as a bit mask;
  the close gesture (the close action held, or two fingers held) ends the game.
- Memory: core memory and frame buffers in PSRAM (`CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY` for the cores'
  static areas), allocated when a game starts and freed when it ends. Saves and states through the storage worker.
- While a game runs, LVGL is paused; never touch LVGL objects from the frame task.
