# esphome-relay-link

ESPHome `dev` plus the relay link of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for relay_link and fixed_containers. Branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/fixed-containers` | fixed_containers: FixedString, TextView, FixedList (fixed-size texts and lists, no heap growth) | - | all |
| `pr/relay-link` | relay_link: the panel's link to the svp_relay Home Assistant integration. No YAML block of its own: a consumer that enables a relay feature loads it (keyboard_layouts `relay`, runtime_language `relay`, screen_capture `relay`, a media player's `extended_api_services` cover art). URL, token and stream port are pushed by the relay through the `relay_link_set` API action (`api: custom_services: true`) and kept in preferences; `http_at()` serves the consumers' plain HTTP sources on the same task | `pr/fixed-containers` | all |
