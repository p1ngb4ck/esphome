# esphome-phone

`sip_phone`: a SIP phone for ESPHome. Registers with a SIP server (a PBX, a home router, a provider), makes and takes
calls with Opus, G.722, PCMA and PCMU, sends DTMF and SIP messages, has contacts, mute, do-not-disturb and auto
answer. Sound comes from a resampler microphone (with echo cancellation, esphome-audio) and goes to any ESPHome
speaker; every state is available to automations, Home Assistant and on-device UIs.

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-phone@main
    components: [sip_phone]
  - source: github://p1ngb4ck/esphome-audio@main
    components: [audio, resampler, speaker, mixer]

sip_phone:
  id: phone
  microphone: call_mic
  speaker: main_speaker
  account:
    server: pbx.local
    username: !secret sip_user
    password: !secret sip_password
  on_incoming_call:
    - logger.log: incoming call

button:
  - platform: template
    name: Answer
    on_press:
      - sip_phone.answer: phone
```

`call_mic` is a `resampler` microphone (16 kHz, ideally with `echo_cancellation`), see
[esphome-audio](https://github.com/p1ngb4ck/esphome-audio/blob/main/CONFIGURATION.md).

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option - account, codecs and Opus settings, ports, timing, contacts,
all triggers, actions, the text sensors and the event entity.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/sip-phone` | `sip_phone`: SIP phone (account registration, Opus / G.722 / PCMA / PCMU, contacts, DTMF, messages), sound from a resampler microphone to a speaker | `pr/resampler-routing` ([esphome-audio](https://github.com/p1ngb4ck/esphome-audio)), `pr/network-priority` ([esphome-network](https://github.com/p1ngb4ck/esphome-network)) | ESP32 |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
