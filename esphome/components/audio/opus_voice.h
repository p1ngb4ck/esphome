#pragma once

#include "esphome/core/defines.h"

#ifdef USE_AUDIO_OPUS_SUPPORT

#include <cstddef>
#include <cstdint>

namespace esphome::audio {

class OpusVoiceEncoder {
 public:
  bool start(uint32_t sample_rate, uint8_t channels, uint32_t frame_ms, int32_t bitrate, int complexity = -1,
             bool fec = false, bool mono = false);
  void stop();
  bool is_started() const { return this->encoder_ != nullptr; }
  size_t frame_samples() const { return this->frame_samples_; }
  size_t frame_bytes() const { return this->frame_samples_ * this->channels_ * sizeof(int16_t); }
  int32_t lookahead() const;
  int encode(const int16_t *pcm, uint8_t *out, size_t out_max);

 protected:
  void *encoder_{nullptr};
  size_t frame_samples_{0};
  uint8_t channels_{1};
};

class OpusVoiceDecoder {
 public:
  bool start(uint32_t sample_rate, uint8_t channels);
  void stop();
  bool is_started() const { return this->decoder_ != nullptr; }
  int decode(const uint8_t *payload, size_t len, int16_t *pcm, size_t max_samples, bool fec = false);
  int conceal(int16_t *pcm, size_t samples);

 protected:
  void *decoder_{nullptr};
};

}

#endif
