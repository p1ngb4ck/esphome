# Configuration reference

The first steps are in [README.md](README.md).

## `runtime_language`

| Option | Default | Description |
|---|---|---|
| `directory` | - | A storage folder with `<name>.yaml` language files. |
| `relay` | `false` | Languages from Home Assistant through the relay link (`/language` lists them, `/language?name=<name>` sends one). Needs `relay_link` ([esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link)). |
| `http_sources` | `[]` | Up to 4 base URLs of web servers answering `<url>/language` and `<url>/language?name=<name>`. Needs `relay_link`. |
| `max_file_size` | `524288` | Largest language file, 1 KB - 4 MB. The loaded language lives in PSRAM. |

The sources are used together: the list is all of them. The chosen language is saved and loaded again at boot.

## What is translated

- LVGL: every static `text:` of a label (also the labels inside buttons).
- Lambdas: `runtime_language::tr("English text")` returns the translation (the literal is collected at build time).
- web_server: entity names.

Changing the language updates every bound label at once.

## Language files

```yaml
"Settings": "Einstellungen"
"Volume": "Lautstärke"
"Wi-Fi": ""
```

One line per text: the English text, then its translation; empty = the English text. A file that misses a text of
the build is refused (the log names the first missing one) and the language shown stays. `write_template` writes
the complete list for the build, with the translations of the language currently loaded.

## Actions

| Action | Options |
|---|---|
| `runtime_language.load` | `name` (a language from the sources) or `path` (a file on a storage), templatable. |
| `runtime_language.list` | Refresh the list of languages from all sources. |
| `runtime_language.unload` | Back to English. |
| `runtime_language.write_template` | `path`: write the template file. |

```yaml
select:
  - platform: template
    name: Language
    options: [English, Deutsch]
    set_action:
      - if:
          condition:
            lambda: 'return x == "Deutsch";'
          then:
            - runtime_language.load:
                name: de
          else:
            - runtime_language.unload:
```
