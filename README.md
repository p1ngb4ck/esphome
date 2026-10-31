# esphome-audio

ESPHome `dev` plus the audio changes of [p1ngb4ck/esphome](https://github.com/p1ngb4ck/esphome) `testing_dev`: the AAC
decoder and eight channels, the decoder's memory, the speaker media player's pipeline end and task priority,
resampler outputs and microphone sources switched at runtime, echo cancellation, i2s_audio / es8311 / es8388 options.
One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`. The code is taken over
unchanged from `testing_dev`: `speaker_media_player_base` identical, the lines of audio, resampler, i2s_audio,
es8311, es8388, mixer, speaker, speaker_source, router and audio_dac assigned by `git blame`; `main` equals
`testing_dev` for all of them.

## Branches

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/audio-aac-eight-channels` | the AAC decoder (output buffer sized from the configuration), `.ogg` as Opus, the re-vendored `speaker_media_player_base` (the speaker auto-loads it), up to eight channels through decoder, mixer and speaker (`num_channels` 1-8); one branch because the eight-channel commits changed the AAC decoder's interface | ESP32 | - |
| `pr/audio-decoder-heap` | AudioDecoder's objects and transfer buffer from a heap its owner gives (AudioHeap), in PSRAM only when asked | ESP32 | `pr/audio-aac-eight-channels` |
| `pr/audio-decoder-discard-output` | `AudioDecoder::discard_output()`: decoded audio not yet written dropped, e.g. before a new position | ESP32 | - |
| `pr/audio-reader-http-tx-buffer` | the HTTP reader's request buffer 2048 bytes (a signed Home Assistant media URL is longer than 512) | ESP32 | - |
| `pr/mixer-source-lock` | mixer: the source speaker's ring buffer and audio source read under the lock | ESP32 | - |
| `pr/speaker-media-player-end` | speaker media_player: the pipeline stopped at the end of a file; resampler `finish()` ends with its input | ESP32 | - |
| `pr/audio-task-priority` | decode and resample tasks at priority 2, above the main loop | ESP32 | `pr/speaker-media-player-end` |
| `pr/resampler-routing` | resampler speaker with a list of outputs, microphone with several sources, both switched at runtime (selects), outputs and sources added and removed at runtime (`usb_audio:` entries), an output in its device's format, microphone `num_channels` | ESP32 | `pr/audio-task-priority` |
| `pr/resampler-aec` | `echo_cancellation` on a microphone source (esp-sr AEC, 16 kHz 16 bit) in its own task | S3, P4 (esp-sr) | `pr/resampler-routing` |
| `pr/resampler-mic-callbacks-capture-time` | microphone data callbacks as the other microphones take them, the source's capture time carried through as upstream #21682; one branch because the capture-time commit rewrote the callback path's signatures | ESP32 | `pr/resampler-aec` |
| `pr/i2s-audio-enable-pin` | i2s_audio speaker `enable_pin` (any GPIO, IO expanders too): the amplifier powered only while the I2S clocks run | ESP32 | - |
| `pr/i2s-audio-nvs` | i2s_audio per-bus `nvs` option: enable, type, lrclk / bclk / mclk, dout, din, enable_pin from the internal NVS | ESP32 | `pr/i2s-audio-enable-pin` |
| `pr/audio-dac-stopping` | `on_audio_stopping` hook: the ES8388 DAC stays muted while its stream stops | ESP32 | `pr/i2s-audio-enable-pin` |
| `pr/es8311-volume-range` | es8311 `volume_min_db` / `volume_max_db`: the range the volume spans, linear in dB | all | - |
| `pr/es8311-dac-reference` | es8311 `dac_reference`: the DAC's playback on the ADC's right slot beside the microphone (echo cancellation reference) | all | - |
| `pr/audio-tasks-loop-core` | i2s_audio, mixer, resampler, speaker tasks pinned to `LOOP_TASK_CORE` | ESP32 | `pr/esp32-loop-task-core` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |

The resampler's `usb_audio:` sources and outputs need `usb_audio` from
[esphome-usb-drivers](https://github.com/p1ngb4ck/esphome-usb-drivers).

## Use

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-audio@main
    components: [esp32, audio, speaker, speaker_media_player_base, mixer, resampler, i2s_audio, es8311, audio_dac]

audio_dac:
  - platform: es8311
    id: es8311_codec
    address: 0x18
    bits_per_sample: 16bit
    sample_rate: 16000
    use_mclk: true
    dac_reference: true
    volume_min_db: -40

speaker:
  - platform: i2s_audio
    i2s_audio_id: i2s_output
    id: speaker_id
    i2s_dout_pin: GPIO09
    enable_pin: GPIO53
    dac_type: external
    sample_rate: 16000
    bits_per_sample: 16bit

microphone:
  - platform: resampler
    id: voip_mic
    sample_rate: 48000
    num_channels: 2
    microphone:
      - microphone: external_mic
        name: "Board"
        channels: [0, 1]
        echo_cancellation:
          mode: voip_low_cost
          reference_channel: 1
```

From the Waveshare ESP32-P4 Pi board's config. `external_components` replaces components only; the ESPHome core of
the build host stays as installed. With the other topics together use the combined branch
`github://p1ngb4ck/esphome@topics-combined`.
