# relay_link: guide for AI agents

The link to the relay integration in Home Assistant - the place for work the MCU cannot do on its own. Everything a
user does still happens on the device; the relay only serves files and renders what the device cannot.

- `RelayLink::create()` makes one instance (PSRAM); consumers get it with `relay_link.get_relay_link()` in codegen.
- `relay_link_set` (API action): URL, token, stream port, kept in the preferences.
- One task (`relay_link`, `StaticTask`) runs the consumers' requests one after another: `submit(fn, ctx, index)`
  queues a job; `fetch_start()` / `fetch_done()` / `fetch_drop()` fetch into the consumer's buffer. A job runs on the
  task and signals completion with an atomic flag or `defer()`; the consumer applies the result on the main loop.
- `http_at(base, ...)`: the consumers' plain HTTP sources over the same task (no relay needed).
- Requests go through ESPHome's `http_request`; no second HTTP client.
- Buffers belong to the consumer and live only for the request.
