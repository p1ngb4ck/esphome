# mtproto: guide for AI agents

A Telegram MTProto 2.0 client on the device.

- `crypto.*`: the MTProto crypto (auth key exchange, AES-IGE, SHA, RSA / DH) on mbedTLS.
- `tl.*`, `schema/`: TL serialization and Telegram's TL schema (`api.json`); generated readers / writers come from
  the schema at build time - update the schema file, not hand-written constructors.
- `mtproto.*`: connection, session, auth key, QR login (`LOGIN_QR`, `qr_url()`), two-step password, updates, the
  state machine (`State`), API actions.
- One TCP connection on its own task; requests and updates are queued; the auth key and session are kept in the
  preferences. Buffers in PSRAM, bounded.
