# esphome-pool

Smaller additions that belong to no other topic: AC current and voltage sensors (ACS712, ZMPT101B) on any voltage
sampler with RMS measurement, burst sampling for the ADS1115 and calibrated multi-sampling for the ESP32 ADC, an
S/PDIF speaker on a single GPIO, select option changes sent to the web server as a change, and two LVGL fixes.

## Getting started

Mains current and power with an ACS712 on an ADS1115:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-pool@main
    components: [ads1115, acs712]

i2c:
  sda: GPIO21
  scl: GPIO22

ads1115:
  - address: 0x48

sensor:
  - platform: ads1115
    id: acs_raw
    multiplexer: A0_GND
    gain: 6.144
  - platform: acs712
    sensor: acs_raw
    name: Current
    model: 20A
    line_voltage: 230
    power:
      name: Power
```

Digital audio out to an amplifier or receiver over S/PDIF:

```yaml
speaker:
  - platform: spdif
    pin: GPIO15
    sample_rate: 48000
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option of `acs712`, `zmpt101b` and the `spdif` speaker, and what the
other changes do.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips |
|---|---|---|
| `pr/adc` | adc: calibrated multi-sampling on the ESP32 | ESP32 |
| `pr/acs712` | `acs712`: ACS712 current sensor (RMS) on a voltage sampler, power with a line voltage | all |
| `pr/zmpt101b` | `zmpt101b`: ZMPT101B AC voltage sensor (RMS) on a voltage sampler | all |
| `pr/ads1115` | ads1115: burst measurements on one channel (for RMS sensors) without a channel switch in between | all |
| `pr/select` | select: option list changes carry a revision; web_server sends only the change | all |
| `pr/spdif` | `spdif`: IEC 60958 (S/PDIF) speaker on one GPIO (the PARLIO peripheral) | ESP32 chips with PARLIO (C5, C6, H2, P4) |
| `pr/lvgl-color-picker-setters` | lvgl color picker: LVGL 9.6's flag setters | all |
| `pr/lvgl-keyboard-text` | lvgl: a keyboard trigger's text is its textarea's (empty without one) | all |

`pr/acs712` and `pr/zmpt101b` build on `pr/adc`. All topics together: `github://p1ngb4ck/esphome@dev`.
`external_components` replaces components only; the ESPHome core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
