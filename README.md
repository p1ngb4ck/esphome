# esphome-audio

Audio changes for ESPHome: the AAC decoder, up to eight channels through decoder, mixer and speaker, one speaker
that plays on several outputs and one microphone fed by several sources - both switched at runtime from Home
Assistant - echo cancellation for calls, an amplifier enable pin, codec options (ES8311 volume range and echo
reference, ES8388), and the media player's pipeline ending cleanly at the end of a file.

## Getting started

A speaker that plays on the board's codec or a USB sound card, switched with a select:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-audio@main
    components: [audio, speaker, mixer, resampler, i2s_audio, speaker_media_player_base]

speaker:
  - platform: i2s_audio
    id: board_speaker
    i2s_dout_pin: GPIO09
    enable_pin: GPIO53
    dac_type: external
  - platform: resampler
    id: main_speaker
    output_speaker:
      - speaker: board_speaker
        name: Board
      - speaker: usb_speaker
        name: USB

select:
  - platform: resampler
    resampler_id: main_speaker
    name: Audio output
```

A microphone with echo cancellation (ESP32-S3 / ESP32-P4), from a codec that puts the playback reference on its
second channel:

```yaml
microphone:
  - platform: resampler
    id: call_mic
    sample_rate: 16000
    microphone:
      - microphone: codec_mic
        name: Board
        channels: [0, 1]
        echo_cancellation:
          mode: voip_low_cost
          reference_channel: 1
```

Use your board's pins; `usb_speaker` comes from `usb_audio`
([esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers)).

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every new option - resampler outputs and sources, echo cancellation
modes, eight channels, `i2s_audio` enable pin and pins from the NVS, ES8311 volume range and DAC reference.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/audio-aac-eight-channels` | the AAC decoder, `.ogg` as Opus, the re-vendored `speaker_media_player_base`, up to eight channels through decoder, mixer and speaker | ESP32 | - |
| `pr/audio-decoder-heap` | AudioDecoder's objects and transfer buffer from a heap its owner gives, in PSRAM when asked | ESP32 | `pr/audio-aac-eight-channels` |
| `pr/audio-decoder-discard-output` | `AudioDecoder::discard_output()`: decoded audio not yet written dropped, e.g. before a seek | ESP32 | - |
| `pr/audio-reader-http-tx-buffer` | the HTTP reader's request buffer 2048 bytes (long signed media URLs) | ESP32 | - |
| `pr/mixer-source-lock` | mixer: the source speaker's ring buffer and audio source read under the lock | ESP32 | - |
| `pr/speaker-media-player-end` | speaker media_player: the pipeline stopped at the end of a file; resampler `finish()` ends with its input | ESP32 | - |
| `pr/audio-task-priority` | decode and resample tasks at priority 2, above the main loop | ESP32 | `pr/speaker-media-player-end` |
| `pr/resampler-routing` | resampler speaker with a list of outputs, microphone with several sources, both switched at runtime (selects), USB audio outputs and sources added and removed at runtime, microphone `num_channels` | ESP32 | `pr/audio-task-priority` |
| `pr/resampler-aec` | `echo_cancellation` on a microphone source (esp-sr AEC, 16 kHz 16 bit) in its own task | S3, P4 | `pr/resampler-routing` |
| `pr/resampler-mic-callbacks-capture-time` | microphone data callbacks as the other microphones take them, the capture time carried through | ESP32 | `pr/resampler-aec` |
| `pr/i2s-audio-enable-pin` | i2s_audio speaker `enable_pin`: the amplifier powered only while the I2S clocks run | ESP32 | - |
| `pr/i2s-audio-nvs` | i2s_audio `nvs`: enable, type and pins of a bus from the internal NVS | ESP32 | `pr/i2s-audio-enable-pin` |
| `pr/audio-dac-stopping` | `on_audio_stopping` hook: the ES8388 DAC stays muted while its stream stops | ESP32 | `pr/i2s-audio-enable-pin` |
| `pr/es8311-volume-range` | es8311 `volume_min_db` / `volume_max_db` | all | - |
| `pr/es8311-dac-reference` | es8311 `dac_reference`: the DAC's playback on the ADC's right slot beside the microphone (echo cancellation reference) | all | - |
| `pr/audio-tasks-loop-core` | i2s_audio, mixer, resampler, speaker tasks pinned to `LOOP_TASK_CORE` | ESP32 | `pr/esp32-loop-task-core` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

Echo cancellation is Espressif's esp-sr AEC. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
