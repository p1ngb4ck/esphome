# esphome-runtime-language

`runtime_language`: the UI language of a device, picked at runtime from a language file. Every static label text of the
LVGL UI and the web server's entity names are translated; lambdas use
`runtime_language::tr("...")`. The English text is the key, so the YAML stays as it is. Languages come from a
storage folder, from Home Assistant or from web servers; the choice is restored at boot. Without a language loaded
everything is English, and no internal RAM is used for translations.

## Getting started

Write the template once - a file with every text of your firmware - then translate it:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-runtime-language@main
    components: [runtime_language, lvgl, web_server, storage]

runtime_language:
  directory: /sdcard/languages

button:
  - platform: template
    name: Write language template
    on_press:
      - runtime_language.write_template:
          path: /sdcard/languages/template.yaml
  - platform: template
    name: German
    on_press:
      - runtime_language.load:
          name: de
```

`template.yaml` lists every text as `"English text": ""`. Copy it to `de.yaml` and fill in the translations; an
empty value keeps the English text. A file that misses a text of the build is refused, and the language shown
stays.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option and action, languages from Home Assistant and HTTP servers,
`tr()` in lambdas, the file format.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/runtime-language` | `runtime_language`: the UI language at runtime - `runtime_language::tr()`, LVGL labels bound to their English text, web_server entity names; languages from `directory`, `relay`, `http_sources`; actions `load` (name or path), `list`, `unload`, `write_template` | `pr/relay-link` ([esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | all |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

`runtime_language` is based on [Alexey Titov](https://github.com/alaltitov)'s ESPHome
[i18n component](https://github.com/alaltitov/esphome) - the YAML language files, `tr()`, switching the language and
the check that refuses a language with missing texts come from there. Thank you, Alexey, for making it public.
Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
