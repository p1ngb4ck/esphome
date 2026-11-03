# Configuration reference

The first steps are in [README.md](README.md).

## `keyboard_layouts`

| Option | Default | Description |
|---|---|---|
| `keyboards` | required with a source | The HID inputs that get layouts. An entry is an ID, or `id`, `name` (shown in the select) and `layout` (the start layout, an XKB name such as `de` or `de(nodeadkeys)`; empty = the input's own). |
| `directory` | - | A storage folder with `<name>.kbl` files. |
| `relay` | `false` | Layouts from Home Assistant through the relay link (`/keyboard` lists them, `/keyboard?layout=<name>` sends one). Needs `relay_link` ([esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link)). |
| `http_sources` | `[]` | Up to 4 base URLs of web servers that answer `<url>/keyboard` (the list) and `<url>/keyboard?layout=<name>`. Needs `relay_link`. |
| `device_select` | - | A select of the connected keyboards (`name`, ... as any select); the layout sensor and changes apply to the selected one. |
| `layout_sensor` | - | A text sensor with the selected keyboard's layout. |

At least one source (`directory`, `relay`, `http_sources`) is required. The list offered is the union of all
sources; a layout is loaded from the storage first, then the relay, then the HTTP sources. Each keyboard's choice is
saved in the preferences and restored when it connects again.

## Layout files

`script/export_hid_layouts.py <folder>` writes every layout of `hid` as `<name>.kbl` plus `index.tsv` (`name<TAB>
description` per line). Serve the same files from a web server for `http_sources`.

## Examples

Layouts from an SD card:

```yaml
keyboard_layouts:
  directory: /sdcard/keyboard
  keyboards:
    - id: keyboard
      name: Desk keyboard
      layout: de
  device_select:
    name: Keyboard
  layout_sensor:
    name: Keyboard layout
```

Layouts from Home Assistant and a web server, no storage:

```yaml
keyboard_layouts:
  relay: true
  http_sources:
    - http://layouts.local:8080
  keyboards:
    - keyboard
    - bt_keyboard
```
