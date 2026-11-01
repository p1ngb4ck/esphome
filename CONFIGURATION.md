# Configuration reference

The first steps are in [README.md](README.md).

## `acs712` sensor

AC (RMS) current from an ACS712 Hall sensor, read through any voltage sampler (`adc`, `ads1115`, ...).

| Option | Default | Description |
|---|---|---|
| `sensor` | required | The voltage sampler (an `adc` or `ads1115` sensor) the ACS712 output is wired to. |
| `model` | `20A` | `5A`, `20A`, `30A` (sets the sensitivity). |
| `sensitivity` | from the model | V per A, 0.001-1.0. |
| `zero_point` | `2.5` | Output voltage at 0 A. |
| `samples` | `100` | Samples per measurement. |
| `sample_duration` | `20ms` | Time the samples span (one mains period at 50 Hz). |
| `line_voltage` | - | A fixed line voltage for the power sensor. |
| `line_voltage_sensor` | - | A voltage sensor (e.g. `zmpt101b`) for the power sensor instead. |
| `power` | - | A power sensor (W). |
| `voltage` | - | A sensor with the measured RMS voltage at the ACS712 output. |

Plus the usual sensor options and `update_interval`.

## `zmpt101b` sensor

AC (RMS) voltage from a ZMPT101B transformer module, read through a voltage sampler.

| Option | Default | Description |
|---|---|---|
| `sensor` | required | The voltage sampler. |
| `frequency` | `50` | Mains frequency, 50 or 60. |
| `sensitivity` | - | Calibration factor, 0.1-100. |
| `zero_point` | `2.5` | Module output at 0 V. |
| `samples` | `200` | Samples per measurement. |
| `sample_duration` | `40ms` | Time the samples span (two mains periods at 50 Hz). |
| `update_interval` | `60s` | |

```yaml
sensor:
  - platform: ads1115
    id: zmpt_raw
    multiplexer: A1_GND
    gain: 4.096
  - platform: zmpt101b
    id: mains_voltage
    sensor: zmpt_raw
    name: Mains voltage
    sensitivity: 520
  - platform: acs712
    sensor: acs_raw
    name: Current
    line_voltage_sensor: mains_voltage
    power:
      name: Power
```

With an `ads1115`, both sensors take their samples as one burst on their channel (no other channel in between).

## `spdif` speaker

S/PDIF (IEC 60958, consumer) on one GPIO through the PARLIO peripheral - to an optical transmitter or a coaxial
output stage.

| Option | Default | Description |
|---|---|---|
| `pin` | required | The output GPIO. |
| `sample_rate` | `48000` | `32000`, `44100` or `48000` (the rates the channel status can announce). |
| `bits_per_sample` | `16bit` | 16 bit only. |
| `num_channels` | `2` | Stereo only. |
| `buffer_duration` | `500ms` | Audio buffer. |
| `task_core` | `-1` | Core of the encoder task; `-1` = any. |

```yaml
speaker:
  - platform: spdif
    id: optical_out
    pin: GPIO15
    sample_rate: 44100
```

## Other changes

- `adc` (ESP32): measurements use the calibration scheme for the voltage, also over multiple samples.
- `ads1115`: `do_burst_measurement()` - several samples on one channel under a lock (used by the RMS sensors).
- `select`: changing the option list counts a revision; the web server sends only the change to open pages.
- `lvgl`: the color picker uses LVGL 9.6's flag setters; a keyboard trigger's `text` is its textarea's text (empty
  without one).
