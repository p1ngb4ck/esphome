# Configuration reference

New and changed options of ESPHome's audio components; everything else is as documented on
[esphome.io](https://esphome.io/components/). The first steps are in [README.md](README.md).

## Eight channels

`num_channels` up to 8 on: `audio` components, `mixer` speakers, `router` speakers (required there), the `speaker`
media player's and `speaker_source`'s pipelines, `resampler` outputs. Decoders and mixers pass every channel
through.

## `resampler` speaker: several outputs

`output_speaker` is a speaker ID as before, or a list of outputs; one plays at a time, chosen at runtime.

| Output option | Default | Description |
|---|---|---|
| `speaker` | required | The output speaker. |
| `name` | required | Its name in the select (up to 32 characters). |
| `sample_rate` | the speaker's | Format the resampler converts to for this output (8000-192000). |
| `bits_per_sample` | the speaker's | 16, 24 or 32. |
| `num_channels` | the speaker's | 1-8. |

An entry `- usb_audio: <usb_audio id>` adds every USB sound card of that driver as an output while it is plugged in.

```yaml
speaker:
  - platform: resampler
    id: main_speaker
    output_speaker:
      - speaker: board_speaker
        name: Board
      - speaker: bt_speaker
        name: Bluetooth
        sample_rate: 44100
      - usb_audio: usb_sound
```

## `resampler` microphone: several sources, echo cancellation

`microphone` takes a list of sources; one is active at a time, chosen at runtime. Every source has the same
`bits_per_sample` and, without `num_channels`, the same channel count.

| Source option | Default | Description |
|---|---|---|
| `microphone` | required | The source microphone (with `channels`, `gain_factor`, `bits_per_sample` as any microphone source). |
| `name` | - | Its name in the select (up to 32 characters). |
| `echo_cancellation` | - | ESP32-S3 / ESP32-P4: esp-sr AEC on this source; needs a 16-bit source with 2 channels (the microphone and the playback reference). |

`echo_cancellation`:

| Option | Default | Description |
|---|---|---|
| `mode` | `voip_low_cost` | `sr_low_cost`, `sr_high_perf`, `voip_low_cost`, `voip_high_perf`, `fd_low_cost`, `fd_high_perf`. |
| `reference_channel` | `1` | Which of the two channels carries the playback reference. |
| `filter_length` | `4` | AEC filter length, 1-8. |

An entry `- usb_audio: <usb_audio id>` adds the microphones of that driver's devices while plugged in, with
`bits_per_sample` (16-32, default 16), `channels` (default `[0]`), `gain_factor` (1-64).

| Microphone option | Default | Description |
|---|---|---|
| `num_channels` | the sources' | 1 or 2: convert every source to this many channels. |

```yaml
microphone:
  - platform: resampler
    id: call_mic
    sample_rate: 16000
    num_channels: 1
    microphone:
      - microphone: codec_mic
        name: Board
        channels: [0, 1]
        echo_cancellation:
          mode: voip_high_perf
      - microphone: usb_cam_mic
        name: Camera
      - usb_audio: usb_sound
```

## `resampler` select

```yaml
select:
  - platform: resampler
    resampler_id: main_speaker
    name: Audio output
  - platform: resampler
    microphone_id: call_mic
    name: Microphone
```

One of `resampler_id` (outputs) and `microphone_id` (sources).

## `i2s_audio`

| Option | Where | Default | Description |
|---|---|---|---|
| `enable_pin` | speaker | - | A pin (also on an IO expander) that powers the amplifier only while the I2S clocks run. |
| `nvs` | bus | - | Read the bus's settings from the internal NVS at boot (one firmware for different boards): `namespace` (required), `enable_default` (`true`), and the key names of `enable`, `type`, `lrclk`, `bclk`, `mclk`, `dout`, `din`, `enable_pin`. |
| `i2s_dout_pin` / `i2s_din_pin` | speaker / microphone | - | Optional with `nvs` (the NVS gives them). |

```yaml
i2s_audio:
  - id: i2s_output
    i2s_lrclk_pin: GPIO10
    i2s_bclk_pin: GPIO12
    i2s_mclk_pin: GPIO13
    nvs:
      namespace: audio

speaker:
  - platform: i2s_audio
    i2s_audio_id: i2s_output
    i2s_dout_pin: GPIO09
    enable_pin: GPIO53
    dac_type: external
```

## `es8311` (audio_dac)

| Option | Default | Description |
|---|---|---|
| `volume_min_db` | `-95.5` | dB at volume 0 %; the volume is linear in dB between min and max. |
| `volume_max_db` | `32.0` | dB at volume 100 %. |
| `dac_reference` | `false` | The DAC's playback on the ADC's right slot, beside the microphone on the left: the reference for echo cancellation. |

```yaml
audio_dac:
  - platform: es8311
    id: codec
    volume_min_db: -40
    dac_reference: true
```

## Other changes

- AAC decoding; `.ogg` files are played as Opus.
- `speaker` media player: the pipeline stops at the end of a file; decode and resample tasks run at priority 2.
- The ES8388 keeps its DAC muted while a stream stops (no click).
- The audio tasks run on the main loop's core (`LOOP_TASK_CORE`).
