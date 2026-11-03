#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32
#ifdef USE_AUDIO_AAC_SUPPORT

#include <cstdint>
#include <cstddef>

// Include esp_audio_codec decoder API
extern "C" {
#include <decoder/esp_audio_dec.h>
#include <esp_audio_types.h>
}

namespace esphome {
namespace audio {
namespace esp_audio_codec_adapter {

/**
 * @brief AAC decoder status codes (matching esp-audio-libs pattern)
 */
enum AACDecoderStatus {
  AAC_DECODER_SUCCESS = 0,
  AAC_DECODER_OUT_OF_DATA = 1,
  AAC_DECODER_SYNC_ERROR = 2,
  AAC_DECODER_ERROR = 3,
};

/**
 * @brief AAC decoder result structure (matching esp-audio-libs pattern)
 */
struct AACDecodeResult {
  AACDecoderStatus status;
  size_t bytes_consumed;
  size_t output_samples;
  int sample_rate;
  int channels;
};

/**
 * @brief Adapter class that wraps esp_audio_codec's audio_element AAC decoder
 *        to provide an API compatible with esp-audio-libs pattern
 */
class AACDecoder {
 public:
  AACDecoder();
  ~AACDecoder();

  /**
   * @brief Decode an AAC frame
   * @param input Pointer to input AAC data
   * @param input_len Length of input data
   * @param output Pointer to output PCM buffer (int16_t samples)
   * @param output_len Bytes available in the output buffer
   * @return AACDecodeResult with status and consumed/produced bytes
   */
  AACDecodeResult decode_frame(const uint8_t *input, size_t input_len, int16_t *output, size_t output_len);

 protected:
  esp_audio_dec_handle_t decoder_{nullptr};
  bool initialized_{false};
  int last_sample_rate_{0};
  int last_channels_{0};
};

}  // namespace esp_audio_codec_adapter
}  // namespace audio
}  // namespace esphome

#endif  // USE_AUDIO_AAC_SUPPORT
#endif  // USE_ESP32
