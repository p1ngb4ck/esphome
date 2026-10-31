#include "jpeg_encoder.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/core/log.h"

namespace esphome::mipi_csi {

ESPHOME_LOG_TAG(TAG, "mipi_csi.jpeg");

static constexpr size_t MIN_OUTPUT_SIZE = 8192;

  this->config_.width = width;
  this->config_.height = height;
  this->config_.src_type = input_format;
  this->config_.sub_sample = sub_sample;
  this->config_.image_quality = quality;
}

size_t JpegEncoder::encode(const uint8_t *frame, size_t frame_length) {
  uint32_t encoded_length = 0;
  esp_err_t err = jpeg_encoder_process(this->engine_, &this->config_, frame, frame_length, this->output_,
                                       this->output_size_, &encoded_length);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Encoding frame failed: %s", esp_err_to_name(err));
    return 0;
  }
  return encoded_length;
}

}  // namespace esphome::mipi_csi

#endif
