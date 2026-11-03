# cert_store: guide for AI agents

TLS material for every TLS client of the device. A consumer never reads certificate files itself.

Python side (`__init__.py`), used by consumers:
- `TLS_SCHEMA`: the options `ca`, `client_certificate`, `client_certificate_key`; extend a client schema with it and
  add `validate_tls` (certificate and key go together). `tls_keys_present(config)` tells whether any is set.
- `tls_refs(config)`: a `cert_store::TlsRefs` expression for the client's setter. Each `CertRef` is `EMBEDDED` (a
  file next to the YAML, its text built in), `STORAGE` (an absolute storage path) or `NAMED` (resolved at runtime).
- A consumer loads `cert_store` only when TLS is enabled (its own `AUTO_LOAD` function), so builds without TLS link
  nothing of it.

C++ side (`cert_store.*`):
- `CertStore::apply_async(conf, out, refs, rng, rng_ctx, done)`: fills an `mbedtls_ssl_config` with CA, client
  certificate and key, step by step (`STEP_CA`, `STEP_CERT`, `STEP_KEY`), and calls `done` on the main loop.
  `TlsMaterial` (CA chain, certificate, key) is owned by the consumer: `init()` before, `free()` when the
  connection is gone. Without a CA the ESP-IDF certificate bundle is attached.
- Storage files are read through storage worker streams (open / read / close callbacks); named certificates are
  tried in `directory`, then through the relay link (`/cert?name=`, Home Assistant) and the `http_sources`
  (`USE_CERT_STORE_LINK`), whose requests run on the relay link's task.
- One read buffer (at most `MAX_BYTES` + 1, PSRAM) exists only while a certificate is being loaded, and is freed
  after parsing. The `CertStore` object itself lives in PSRAM.
- One apply at a time; a consumer that needs TLS waits for its `done` before connecting.
