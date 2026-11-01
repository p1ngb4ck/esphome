# esphome-browser

ESPHome `dev` plus the Browser app of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for browser and remote_desktop. An app of its own (one repo per app); branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/browser` | browser: a VNC / SPICE server that runs a browser, opened through remote_desktop (`browser.open` / `close` / `is_open`) | `pr/remote-desktop` ([esphome-remote-desktop](https://github.com/p1ngb4ck/esphome-remote-desktop)) | P4 |
