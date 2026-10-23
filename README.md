# mipi_dsi: generic commands for bridge chips

New display option `command_mode`: `dcs` (default, unchanged behaviour) or `generic`.

With `generic` the init sequence is sent as generic write packets (short writes for 0 and 1 argument, long writes
otherwise), as DSI bridge chips such as the TC358762 expect them:

- no acknowledge request after each command,
- no DCS soft reset,
- none of the DCS commands mipi_dsi otherwise adds (PIXFMT, MADCTL, INVON / INVOFF, SLPOUT, DISPON).

Models can set it as a default.

```yaml
display:
  - platform: mipi_dsi
    command_mode: generic
```

Part of [rpi7-v1-upstream](https://github.com/p1ngb4ck/esphome/tree/rpi7-v1-upstream).
