#include "jpeg_encoder.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <algorithm>
#include "esphome/core/log.h"

namespace esphome::mipi_csi {

ESPHOME_LOG_TAG(TAG, "mipi_csi.jpeg");

/// Encoder engines need a per-frame timeout. 5 s is far longer than any supported frame takes.
static constexpr int ENCODE_TIMEOUT_MS = 5000;
/// Smallest output buffer, so that tiny frames still have room for the headers.
static constexpr size_t MIN_OUTPUT_SIZE = 8192;

size_t JpegEncoder::output_size_for(size_t frame_size) {
  // A JPEG is expected to be well under the raw frame size, and three quarters of it is the
  // headroom Espressif's own examples use. Encoding fails outright if a frame ever exceeds it.
  return std::max(frame_size * 3 / 4, MIN_OUTPUT_SIZE);
}

bool JpegEncoder::open(uint16_t width, uint16_t height, jpeg_enc_input_format_t input_format,
                       jpeg_down_sampling_type_t sub_sample, uint8_t quality, uint8_t *output, size_t output_size) {
  jpeg_encode_engine_cfg_t engine_config{};
  engine_config.timeout_ms = ENCODE_TIMEOUT_MS;
  esp_err_t err = jpeg_new_encoder_engine(&engine_config, &this->engine_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Encoder engine setup failed: %s", esp_err_to_name(err));
    this->engine_ = nullptr;
    return false;
  }
  this->output_ = output;
  this->output_size_ = output_size;
  this->config_.width = width;
  this->config_.height = height;
  this->config_.src_type = input_format;
  this->config_.sub_sample = sub_sample;
  this->config_.image_quality = quality;
  ESP_LOGD(TAG, "Encoder ready: %ux%u, quality %u, %zu byte output buffer", width, height, quality, output_size);
  return true;
}

void JpegEncoder::close() {
  if (this->engine_ != nullptr) {
    jpeg_del_encoder_engine(this->engine_);
    this->engine_ = nullptr;
  }
  this->output_ = nullptr;
  this->output_size_ = 0;
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
