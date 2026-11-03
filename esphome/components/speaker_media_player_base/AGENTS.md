# speaker_media_player_base: guide for AI agents

The speaker media player's pipeline (`audio_pipeline.*`, `speaker_media_player.*`), vendored as its own component
so other media players (USB audio, Bluetooth, video players) build on the same pipeline. The `speaker` media player
auto-loads it.

- The pipeline: reader (HTTP / file / flash), decoder (FLAC, MP3, WAV, Opus, AAC), resampler, speaker; up to eight
  channels.
- The pipeline stops at the end of a file (no trailing idle stream); the decoder's output can be discarded before a
  seek (`discard_output()`).
- Decoder objects and the transfer buffer come from the heap the owner gives (`AudioHeap`), PSRAM when asked.
- Keep it in sync with upstream's speaker media player where the behaviour is the same; differences belong to the
  branches of this topic.
