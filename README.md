# esphome-phone

ESPHome `dev` plus the SIP phone app of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for sip_phone. An app of its own (one repo per app); branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/sip-phone` | sip_phone: SIP phone (account registration, Opus / G.722 / PCMA / PCMU, contacts, DTMF), sound from a resampler microphone to a speaker | `pr/resampler-routing` ([esphome-audio](https://github.com/p1ngb4ck/esphome-audio)), `pr/network-priority` ([esphome-network](https://github.com/p1ngb4ck/esphome-network)) | ESP32 |
