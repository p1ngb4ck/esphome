# mipi_dsi: DSI link options

New display options, all optional; the defaults are IDF's current behaviour, so existing configs and models are
unchanged.

| Option | Default | Effect |
|---|---|---|
| `frame_ack` | `true` | Bus turnaround (acknowledge) at the end of every frame. Off for peripherals that never answer a turnaround. |
| `continuous_clock` | `false` | Keeps the clock lane in high speed, for bridges that take their pixel clock from it. |
| `eotp` | `true` | End of Transmission packet after each high-speed transmission. |
| `lp_horizontal_blanking` | `true` | Low power allowed in the horizontal back and front porch. |
| `lp_vertical_active` | `true` | Low power allowed inside the active lines. |

Models can set these as defaults.

```yaml
display:
  - platform: mipi_dsi
    frame_ack: false
    continuous_clock: true
    eotp: false
    lp_horizontal_blanking: false
    lp_vertical_active: false
```

Part of [rpi7-v1-upstream](https://github.com/p1ngb4ck/esphome/tree/rpi7-v1-upstream).
