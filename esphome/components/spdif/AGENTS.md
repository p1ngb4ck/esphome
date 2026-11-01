# spdif: guide for AI agents

An S/PDIF (IEC 60958 consumer) speaker on one GPIO.

- `speaker/spdif_speaker.*`: the speaker - a ring buffer filled by `play()`, an encoder task (core `task_core`) that
  turns samples into biphase-mark subframes (`iec60958.h`: preambles, channel status with the sample rate code,
  parity) and streams them through a PARLIO TX unit with one data line.
- Only the formats S/PDIF consumer frames carry without requantising: 16 bit stereo at 32 / 44.1 / 48 kHz
  (`audio.set_stream_limits`), so upstream components resample to it.
- The PARLIO unit and the buffers exist while the speaker runs and are released when it stops.
