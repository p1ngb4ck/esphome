# esphome-runtime-language

ESPHome `dev` plus the runtime language of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for runtime_language and lvgl's label.py; web_server carries only the language hunks. Branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/runtime-language` | runtime_language: the UI language at runtime - `runtime_language::tr()`, LVGL labels bound to their English text, web_server entity names; languages from sources in parallel: `directory` (`<name>.yaml` on storage), `relay: true`, `http_sources` (`<base>/language`, `<base>/language?name=<name>`); actions `load` (name or path), `list`, `unload`, `write_template` | `pr/relay-link` ([esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | all |
