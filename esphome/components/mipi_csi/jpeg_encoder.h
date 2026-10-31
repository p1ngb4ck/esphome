#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <driver/jpeg_encode.h>

namespace esphome::mipi_csi {

/// Wrapper around the ESP32-P4 hardware JPEG encoder.
///
class JpegEncoder {
 public:
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
