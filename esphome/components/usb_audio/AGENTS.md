# usb_audio: guide for AI agents

USB Audio Class devices (UAC 1 / 2) as speakers, microphones, DAC and media player. Reworked from upstream: several
devices per driver, a format per device, streams only while used. Read `esphome/components/usb_host/AGENTS.md`
(isochronous streams, channel budget) first.

- `usb_audio.*`: `USBAudioClient` - descriptor parsing (`uac_types.h`), format choice (the nearest the device
  offers to the configured `speaker` / `microphone` format), isochronous OUT / IN streams with the optional feedback
  endpoint, class requests (sample rate, volume, mute) on a control task, `stream_idle_timeout`, channel pairs of
  multichannel devices, volume curve and per-device quirks.
- Platforms: `speaker/`, `microphone/`, `audio_dac/`, `media_player/` (ESPHome's speaker media player on a USB
  device), each bound by `usb_audio_id`.

Rules:
- Stream fill / packet callbacks run on the USB task and only move data between the ring buffers and the USB
  packets; class requests never run there (they block) - they go to the control task.
- A stream opens when audio flows and closes after `stream_idle_timeout`; its ring buffer is allocated with the
  stream and freed with it.
- Up to 8 channels pass through unchanged; mixing and resampling belong to the audio components (esphome-audio).
