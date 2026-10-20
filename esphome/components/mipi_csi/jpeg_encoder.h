#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <driver/jpeg_encode.h>

namespace esphome::mipi_csi {

/// Wrapper around the ESP32-P4 hardware JPEG encoder.
///
/// The output buffer belongs to the caller (taken for the stream, given back after it), so only one
/// encoded frame can be in flight at a time. The engine exists between open() and close().
class JpegEncoder {
 public:
  /// The output size open() needs for frames of @p frame_size bytes (3/4 of a raw frame, at least 8 KiB).
  static size_t output_size_for(size_t frame_size);

  /// Creates the encoder engine; @p output (DMA-capable, cache-line aligned) receives every image.
  bool open(uint16_t width, uint16_t height, jpeg_enc_input_format_t input_format, jpeg_down_sampling_type_t sub_sample,
            uint8_t quality, uint8_t *output, size_t output_size);
  /// Deletes the engine; the output buffer stays the caller's.
  void close();

  /// Encodes one frame into the output buffer.
  /// @return Number of JPEG bytes written to the output buffer, or 0 on failure.
  size_t encode(const uint8_t *frame, size_t frame_length);

  /// Returns the buffer holding the most recently encoded image.
  uint8_t *get_output_buffer() const { return this->output_; }

 protected:
  jpeg_encoder_handle_t engine_{nullptr};
  jpeg_encode_cfg_t config_{};
  uint8_t *output_{nullptr};
  size_t output_size_{0};
};

}  // namespace esphome::mipi_csi

#endif
