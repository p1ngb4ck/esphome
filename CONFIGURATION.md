# Configuration reference

The first steps are in [README.md](README.md). ESP32-P4 only.

## `emulator`

| Option | Default | Description |
|---|---|---|
| `lvgl_id` | required | The LVGL instance whose display's video output shows the game. |
| `systems` | required | The systems built in, each with its game folder on a storage: `nes`, `gb`, `sms`, `pce`, `lynx`, `snes`, `md`, `gw`, `msx`. |
| `bios_dir` | - | Folder with BIOS files; required for `msx` (`MSX2.ROM`, `MSX2EXT.ROM`, ...). |
| `min_scale` | `1.0` | Smallest picture scale a system may need on this screen (1/16 - 16); systems whose native picture would need less are left out at build time. |
| `speaker` | - | The sound output. |
| `hid_ids` | `[]` | HID inputs (gamepads, keyboards) that play. |
| `keys` | per button | Keys per button: `up`, `down`, `left`, `right`, `a`, `b`, `x`, `y`, `l`, `r`, `start`, `select`, each a list of keys (names or `page:usage`). Defaults: the gamepad's d-pad and buttons plus keyboard keys (arrows, `X`, `Z`, ...). |
| `menu_keys` | `[BUTTON_11]` | Keys that open the in-game menu. |
| `key_bindings_id` | - | LVGL key bindings: the game's keys and the close action come from there. |
| `close_action` | `home` | The key binding action that ends the game when held (Back stays a normal key). |
| `touchscreen_id` | - | The touchscreen: on-screen play and the two-finger hold that ends the game. |
| `link` | - | Two devices linked over the network (Game Boy link and similar): `port` (`6666`), `peers` (host names / addresses offered). |
| `on_started` / `on_paused` / `on_ended` | - | Game events. |
| `on_players_changed` / `on_players_closed` | - | The players dialog changed / closed. |

```yaml
emulator:
  id: games
  lvgl_id: lvgl_main
  speaker: main_speaker
  hid_ids: [usb_pad, bt_pad]
  key_bindings_id: key_bindings
  touchscreen_id: touch
  bios_dir: /sdcard/games/bios
  systems:
    nes: /sdcard/games/nes
    gb: /sdcard/games/gb
    snes: /sdcard/games/snes
    msx: /sdcard/games/msx
  keys:
    a: [BUTTON_2, X]
    b: [BUTTON_1, Z]
  link:
    port: 6666
  on_ended:
    - lvgl.page.show: games_page
```

## Actions

| Action | Options |
|---|---|
| `emulator.start` | `path` (the game file, templatable); the system follows from its folder or its extension. |
| `emulator.stop`, `emulator.pause`, `emulator.resume` | - |
| `emulator.save_state` / `emulator.load_state` | `slot` 1-4 (templatable), written / read through the storage worker. |
| `emulator.players` | `path`: open the players dialog for a game. |
| `emulator.players_set` | `index` 0-3, `seat` 0-2: put an input on a seat. |
| `emulator.players_start` / `emulator.players_cancel` | Start with the chosen seats / close the dialog. |
| `emulator.link_host` | Wait for another device to join (on `link: port`). |
| `emulator.link_join` | `host` (templatable): join the link of another device. |
| `emulator.link_off` | End the link. |

```yaml
- emulator.save_state:
    id: games
    slot: 1
```
