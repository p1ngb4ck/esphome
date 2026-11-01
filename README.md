# esphome-messenger

ESPHome `dev` plus the messenger app of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for messenger, mtproto and sip_phone. An app of its own (one repo per app); branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/mtproto` | mtproto: Telegram MTProto client (crypto, TL schema) | `pr/network-priority` ([esphome-network](https://github.com/p1ngb4ck/esphome-network)) | ESP32 |
| `pr/messenger` | messenger: messenger app (Telegram account through mtproto, calls through sip_phone and the camera's H.264 stream) | `pr/mtproto`, `pr/sip-phone` ([esphome-phone](https://github.com/p1ngb4ck/esphome-phone)), `pr/camera-encoder-h264` ([esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | ESP32 (camera H.264: P4) |
