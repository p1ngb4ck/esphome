# esphome-remote-desktop

ESPHome `dev` plus the remote desktop app of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`. One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over unchanged from `testing_dev`; `main` equals `testing_dev` for remote_desktop and display. An app of its own (one repo per app); branches of other topic repos it builds on are merged into the branch that needs them.

## Branches

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/remote-desktop` | remote_desktop: VNC (RFB, Tight JPEG through the HW decoder) and SPICE (the guest's spice-streaming-agent MJPEG, JPEG images through the HW decoder) on a page canvas of the video output, no relay; keys / mouse / touches, cursor, SPICE sound, USB redirection (usbredir; USB/IP during VNC), saved connections (preferences or a connections file), probing (`protocol: auto`), close gestures | `pr/display-video-output-page-draw` ([esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)), `pr/usb-host-refactor` ([esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host)) | P4 |
