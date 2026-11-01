# Configuration reference

The first steps are in [README.md](README.md).

- [HID input options](#hid-input-options) - every transport (`usb_hid`, `bt_hid`, `ble_client_hid`)
- [Actions](#actions) - rumble, player LEDs, light bar
- [Sensors](#sensors) - binary sensor, event, sensor, text sensor
- [LVGL](#lvgl) - `hid_keyboards`, `hid_pointers`, `remote_input`, `key_bindings`

## Keys

A key is given by name (`ENTER`, `F1`, `A`, `VOLUME_UP`, `MENU_UP`, `AC_BACK`, ...), by HID `page:usage`
(`0x0C:0xE9`, for remote buttons without a name) or by a keyboard usage number.

## HID input options

Every HID transport takes these options in its entries.

| Option | Default | Description |
|---|---|---|
| `layout` | `us` | Keyboard layout, XKB naming: `de`, `fr`, `gb`, `de(nodeadkeys)`, ... (about a hundred, from xkeyboard-config). Or a mapping: `base` (a layout name) and `keys` (key name -> up to 4 levels of characters) to change single keys. |
| `num_lock` | `true` | Num Lock on at connect. |
| `on_key` | - | A key pressed or released: `key` (string), `pressed` (bool). With `key:` only that key. |
| `on_text` | - | Text a key press typed, through the layout: `text`. |
| `on_pointer` | - | Mouse / pointer motion: `dx`, `dy`, `wheel`, `pan`, `buttons` (bitmask). |
| `on_control` | - | A gamepad / remote control changed: `control` (name), `value`. |
| `api_actions` | `false` | API actions to send keys / text to this input from Home Assistant. |

```yaml
usb_hid:
  - id: keyboard
    layout:
      base: de
      keys:
        GRAVE: ["^", "°"]
    on_key:
      - key: ESCAPE
        then:
          - logger.log: escape
    on_text:
      - logger.log:
          format: "typed %s"
          args: [text.c_str()]
  - id: gamepad
    on_control:
      - logger.log:
          format: "%s = %d"
          args: [control.c_str(), value]
```

## Actions

Gamepads with the matching feature:

```yaml
- hid.rumble:
    id: gamepad
    strong: 80%
    weak: 40%
    duration: 300ms
- hid.set_player_led:
    id: gamepad
    player: 1
- hid.set_light_bar:
    id: gamepad
    red: 0%
    green: 50%
    blue: 100%
```

| Action | Options |
|---|---|
| `hid.rumble` | `strong` (`100%`), `weak` (`100%`), `duration` (`0ms` = until the next rumble) |
| `hid.set_player_led` | `player` 0-5 (required) |
| `hid.set_light_bar` | `red`, `green`, `blue` (required, percentages) |

## Sensors

All take `hid_id` (the input).

| Platform | Options | Value |
|---|---|---|
| `binary_sensor` | `key` (required) | The key is held. |
| `event` | `keys` (required, 1-255 keys) | An event per press, type = the key. |
| `sensor` | `control` (required) | A gamepad / remote control's value (axis, trigger). |
| `text_sensor` | `type` (required): `last_key`, `text`, `line` | The last key's name; what the last press typed; a typed line, published on Enter (barcode scanners). |

```yaml
text_sensor:
  - platform: hid
    hid_id: keyboard
    type: line
    name: Barcode

binary_sensor:
  - platform: hid
    hid_id: remote
    key: AC_HOME
    name: Home button
```

## LVGL

### `hid_keyboards`

Keyboards (and remotes, gamepads) that operate LVGL through key bindings. A list; an entry can be just the ID.

| Option | Default | Description |
|---|---|---|
| `hid_id` | required | The HID input. |
| `group` | - | The LVGL group the keys go to. |
| `long_press_time` | `400ms` | Hold time for a long press. |
| `long_press_repeat_time` | `100ms` | Repeat interval while held. |

### `hid_pointers`

Mice, touchpads and gamepad sticks as an LVGL pointer with a cursor; the left button presses, the wheel scrolls.

| Option | Default | Description |
|---|---|---|
| `hid_id` | required | One input or a list. |
| `cursor` | - | An image ID as the cursor. |
| `hide_after` | always shown | Hide the cursor after this long without motion. |
| `long_press_time` / `long_press_repeat_time` | `400ms` / `100ms` | As above. |

### `remote_input`

LVGL operated from outside - Home Assistant or scripts send pointer events, key binding actions and text. Keys also
fire their action while LVGL is paused. `long_press_time` / `long_press_repeat_time` as above.

```yaml
lvgl:
  remote_input:
    id: lv_remote
```

### `key_bindings`

Which keys trigger which action. Built-in actions with default keys:

| Action | Default keys |
|---|---|
| `up` / `down` / `left` / `right` | arrows, `MENU_UP` / `MENU_DOWN` / `MENU_LEFT` / `MENU_RIGHT`, remote d-pads |
| `next` / `prev` | `TAB` / `SHIFT+TAB` |
| `enter` | `ENTER`, `KP_ENTER`, `SELECT` |
| `back` | `ESCAPE`, `MENU_ESCAPE`, `AC_BACK` |
| `home` / `end` | `HOME`, `AC_HOME` / `END` |
| `page_up` / `page_down` | `PAGE_UP` / `PAGE_DOWN` |
| `backspace` / `delete` | `BACKSPACE` / `DELETE` |

| Option | Default | Description |
|---|---|---|
| `actions` | `[]` | Own actions: `name` (required), `keys` (key combos, up to 3 keys with `+`: `CTRL+S`; modifiers `SHIFT`, `CTRL`, `ALT`, `GUI`), `then` (the automation). A built-in name with `keys` changes its keys. |
| `file` | - | A key file on a storage: learned and added bindings are kept there. |
| `on_learned` | - | A key was learned. |

```yaml
lvgl:
  hid_keyboards:
    - keyboard
  key_bindings:
    id: key_bindings
    file: /sdcard/keys.txt
    actions:
      - name: back
        keys: [ESCAPE, AC_BACK, BACKSPACE]
      - name: save
        keys: [CTRL+S]
        then:
          - script.execute: save_settings
```

Actions:

| Action | Options |
|---|---|
| `lvgl.key_bindings.learn` | `action` (required), `timeout` (`10s`): the next key pressed is bound to the action. |
| `lvgl.key_bindings.add` | `action`, `key` (both required) |
| `lvgl.key_bindings.remove` | `key` (required) |
| `lvgl.key_bindings.clear` | `action` (required): remove every key of an action. |
