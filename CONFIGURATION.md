# Configuration reference

The first steps are in [README.md](README.md).

## `relay_link`

No options of its own; it is loaded by the components that use it. It needs `api:` with `custom_services: true` (the
relay integration calls the API action `relay_link_set` with `url`, `token` and `port`) and uses `http_request`.

What the consumers ask the relay for:

| Consumer | Option | Requests |
|---|---|---|
| `keyboard_layouts` | `relay: true` | `/keyboard` (list), `/keyboard?layout=<name>` |
| `runtime_language` | `relay: true` | `/language` (list), `/language?name=<name>` |
| `cert_store` | `relay: true` | `/cert?name=<name>` |
| `screen_capture` | `relay: true`, `target: relay` | screenshots and screencasts sent to the relay |
| media player | `extended_api_services`, `queue: relay: true` | cover art, queue items |
| `stream_receiver` | - | the stream port pushed with `relay_link_set` |

### Plain HTTP sources

The same consumers take `http_sources`: up to 4 base URLs of ordinary web servers that answer the same paths
(`<url>/keyboard`, `<url>/language?name=de`, ...). No relay integration is needed for those; the requests run on the
relay link's task as well.

```yaml
runtime_language:
  http_sources:
    - http://files.local:8080/esphome
```

## `fixed_containers` (for component authors)

| Type | What |
|---|---|
| `FixedString<N>` | A string with a fixed capacity, no heap allocation; converts to and compares with `std::string` / `const char *`. |
| `TextView` | A non-owning view on text. |
| `FixedList<T>` | A list in PSRAM whose capacity is set once (`reserve()`), then never grows. |

Use them where a component keeps texts or lists for its whole life and must not grow memory at runtime.
