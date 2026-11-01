# resampler: guide for AI agents

Reworked from upstream: the resampler speaker routes to one of several outputs, the resampler microphone takes one
of several sources, both switched at runtime; echo cancellation on a source.

- `speaker/`: `ResamplerSpeaker` - the output list (each with its own format), the active output, USB audio outputs
  added / removed at runtime (`usb_audio:` entries), `finish()` ends with its input. The resample task runs at
  priority 2 on `LOOP_TASK_CORE`.
- `microphone/`: `ResamplerMicrophone` - the source list, channel conversion (`num_channels`), the capture time of
  the source carried through to the data callbacks, `echo_cancellation` (esp-sr AEC, 16 kHz / 16 bit, microphone +
  reference channel) in its own task.
- `select/`: `OutputSelect` / `SourceSelect`.

Rules:
- Switching an output or a source happens on the main loop; the audio tasks see the change at a buffer boundary,
  never mid-buffer.
- Buffers belong to the running stream: allocated when the stream starts, freed when it stops; PSRAM for ring
  buffers where the chip has it.
- The AEC task exists only while an AEC source is active.
- Task cores and priorities of the audio path are a tested layout; do not move them.
