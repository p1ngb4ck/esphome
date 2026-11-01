# esphome-relay-link

`relay_link`: the device's link to a relay integration in Home Assistant (svp_relay), for what an MCU cannot do on
its own - keyboard layouts, languages, certificates and pictures served by Home Assistant, cover art, screen
captures sent to Home Assistant, streams rendered on the server. And `fixed_containers`: fixed-size strings and
lists for components that must not grow memory at runtime.

`relay_link` has no YAML block of its own: a component that uses a relay feature loads it (`keyboard_layouts:
relay: true`, `runtime_language: relay: true`, `cert_store: relay: true`, `screen_capture: relay: true`, a media
player's `extended_api_services` or `queue: relay: true`). URL, token and stream port are pushed by the relay
integration through the API action `relay_link_set` and kept in the preferences.

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-relay-link@main
    components: [relay_link, fixed_containers]
  - source: github://p1ngb4ck/esphome-keyboard-layouts@main
    components: [keyboard_layouts]

api:
  custom_services: true

keyboard_layouts:
  relay: true
  keyboards:
    - keyboard
```

The relay integration then calls `relay_link_set` on the device once; from then on the device reaches it on its own.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: what the relay link serves, the plain HTTP sources (`http_sources`) the
consumers take as an alternative, and `fixed_containers` for component authors.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/fixed-containers` | `fixed_containers`: `FixedString`, `TextView`, `FixedList` (fixed-size texts and lists, no heap growth) | - | all |
| `pr/relay-link` | `relay_link`: the link to the relay integration (URL, token and stream port through `relay_link_set`, kept in the preferences), one task for the consumers' requests, `http_at()` for their plain HTTP sources | `pr/fixed-containers` | all |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
