#include "jpeg_decoder.h"
#ifdef USE_RUNTIME_IMAGE_JPEG

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_IDF
#include "esp_task_wdt.h"
#endif

#if defined(USE_HWJPG)
#include "esp_heap_caps.h"
#endif

static const char *const TAG = "image_decoder.jpeg";

namespace esphome::runtime_image {

int HOT JpegDecoder::decode(uint8_t *buffer, size_t size) {
  // JPEG decoder requires complete data
  // If we know the expected size, wait for it
  if (this->expected_size_ > 0 && size < this->expected_size_) {
    ESP_LOGV(TAG, "Download not complete. Size: %zu/%zu", size, this->expected_size_);
    return 0;
  }
  return this->decode_backend_<JPEG_BACKEND>(buffer, size);
}

#if defined(USE_HWJPG) || defined(USE_NEWJPEG)
void JpegDecoder::draw_rgb888_buffer_(const uint8_t *rgb, uint32_t width, uint32_t height) {
  size_t position = 0;
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      uint8_t r = rgb[position++];
      uint8_t g = rgb[position++];
      uint8_t b = rgb[position++];
      this->draw(x, y, 1, 1, Color(r, g, b, 255));
    }
  }
}
#endif

#if defined(USE_HWJPG)

// ESP32-P4: hardware JPEG codec (esp_driver_jpeg).
template<> int JpegDecoder::decode_backend_<JpegBackend::HW_P4>(uint8_t *buffer, size_t size) {
  jpeg_decode_picture_info_t info;
  if (jpeg_decoder_get_info(buffer, size, &info) != ESP_OK) {
    ESP_LOGE(TAG, "Could not parse JPEG header");
    return DECODE_ERROR_INVALID_TYPE;
  }
  if (!this->set_size(static_cast<int>(info.width), static_cast<int>(info.height))) {
    return DECODE_ERROR_OUT_OF_MEMORY;
  }

  jpeg_decode_engine_cfg_t eng_cfg = {};
  eng_cfg.intr_priority = 0;
  eng_cfg.timeout_ms = 1000;
  jpeg_decoder_handle_t decoder = nullptr;
  if (jpeg_new_decoder_engine(&eng_cfg, &decoder) != ESP_OK) {
    ESP_LOGE(TAG, "Could not create hardware JPEG decoder engine");
    return DECODE_ERROR_INTERNAL_DECODER_ERROR;
  }

  jpeg_decode_memory_alloc_cfg_t out_alloc_cfg{};
  out_alloc_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  size_t out_capacity = 0;
  auto *outbuf = static_cast<uint8_t *>(
      jpeg_alloc_decoder_mem(static_cast<size_t>(info.width) * info.height * 3, &out_alloc_cfg, &out_capacity));
  if (outbuf == nullptr) {
    ESP_LOGE(TAG, "Could not allocate JPEG output buffer");
    jpeg_del_decoder_engine(decoder);
    return DECODE_ERROR_OUT_OF_MEMORY;
  }

  jpeg_decode_cfg_t decode_cfg{};
  decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
  decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

  uint32_t out_size = 0;
  esp_err_t err = jpeg_decoder_process(decoder, &decode_cfg, buffer, static_cast<uint32_t>(size), outbuf,
                                        static_cast<uint32_t>(out_capacity), &out_size);
  jpeg_del_decoder_engine(decoder);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Hardware JPEG decode failed: %d", err);
    heap_caps_free(outbuf);
    return DECODE_ERROR_INTERNAL_DECODER_ERROR;
  }

  this->draw_rgb888_buffer_(outbuf, info.width, info.height);
  heap_caps_free(outbuf);
  this->decoded_bytes_ = size;
  return size;
}

#elif defined(USE_NEWJPEG)

// ESP32-S2/S3: esp_new_jpeg (SIMD-optimized software decoder).
template<> int JpegDecoder::decode_backend_<JpegBackend::NEW_JPEG>(uint8_t *buffer, size_t size) {
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB888;
  jpeg_dec_handle_t decoder = nullptr;
  if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "Could not create esp_new_jpeg decoder");
    return DECODE_ERROR_INTERNAL_DECODER_ERROR;
  }

  jpeg_dec_io_t io{};
  io.inbuf = buffer;
  io.inbuf_len = static_cast<int>(size);

  jpeg_dec_header_info_t header_info;
  if (jpeg_dec_parse_header(decoder, &io, &header_info) != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "Could not parse JPEG header");
    jpeg_dec_close(decoder);
    return DECODE_ERROR_INVALID_TYPE;
  }
  if (!this->set_size(header_info.width, header_info.height)) {
    jpeg_dec_close(decoder);
    return DECODE_ERROR_OUT_OF_MEMORY;
  }

  int outbuf_len = 0;
  jpeg_dec_get_outbuf_len(decoder, &outbuf_len);
  auto *outbuf = static_cast<uint8_t *>(jpeg_calloc_align(outbuf_len, 16));
  if (outbuf == nullptr) {
    ESP_LOGE(TAG, "Could not allocate JPEG output buffer");
    jpeg_dec_close(decoder);
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  io.outbuf = outbuf;

  jpeg_error_t process_err = jpeg_dec_process(decoder, &io);
  jpeg_dec_close(decoder);
  if (process_err != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "esp_new_jpeg decode failed: %d", process_err);
    jpeg_free_align(outbuf);
    return DECODE_ERROR_INTERNAL_DECODER_ERROR;
  }

  this->draw_rgb888_buffer_(outbuf, header_info.width, header_info.height);
  jpeg_free_align(outbuf);
  this->decoded_bytes_ = size;
  return size;
}

#else

/**
 * @brief Callback method that will be called by the JPEGDEC engine when a chunk
 * of the image is decoded.
 *
 * @param jpeg  The JPEGDRAW object, including the context data.
 */
static int draw_callback(JPEGDRAW *jpeg) {
  ImageDecoder *decoder = (ImageDecoder *) jpeg->pUser;

  // Some very big images take too long to decode, so feed the watchdog on each callback
  // to avoid crashing if the executing task has a watchdog enabled.
#ifdef USE_ESP_IDF
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
#endif
    App.feed_wdt();
#ifdef USE_ESP_IDF
  }
#endif
  size_t position = 0;
  size_t height = static_cast<size_t>(jpeg->iHeight);
  size_t width = static_cast<size_t>(jpeg->iWidth);
  for (size_t y = 0; y < height; y++) {
    for (size_t x = 0; x < width; x++) {
      auto rg = decode_value(jpeg->pPixels[position++]);
      auto ba = decode_value(jpeg->pPixels[position++]);
      Color color(rg[1], rg[0], ba[1], ba[0]);

      if (!decoder) {
        ESP_LOGE(TAG, "Decoder pointer is null!");
        return 0;
      }
      decoder->draw(jpeg->x + x, jpeg->y + y, 1, 1, color);
    }
  }
  return 1;
}

// Other ESP32 variants and host: JPEGDEC (bitbank2, software).
template<> int JpegDecoder::decode_backend_<JpegBackend::JPEGDEC>(uint8_t *buffer, size_t size) {
  if (!this->jpeg_.openRAM(buffer, size, draw_callback)) {
    ESP_LOGE(TAG, "Could not open image for decoding: %d", this->jpeg_.getLastError());
    return DECODE_ERROR_INVALID_TYPE;
  }
  auto jpeg_type = this->jpeg_.getJPEGType();
  if (jpeg_type == JPEG_MODE_INVALID) {
    ESP_LOGE(TAG, "Unsupported JPEG image");
    return DECODE_ERROR_INVALID_TYPE;
  } else if (jpeg_type == JPEG_MODE_PROGRESSIVE) {
    ESP_LOGE(TAG, "Progressive JPEG images not supported");
    return DECODE_ERROR_INVALID_TYPE;
  }
  ESP_LOGD(TAG, "Image size: %d x %d, bpp: %d", this->jpeg_.getWidth(), this->jpeg_.getHeight(), this->jpeg_.getBpp());

  this->jpeg_.setUserPointer(this);
  this->jpeg_.setPixelType(RGB8888);
  if (!this->set_size(this->jpeg_.getWidth(), this->jpeg_.getHeight())) {
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  if (!this->jpeg_.decode(0, 0, 0)) {
    auto error = this->jpeg_.getLastError();
    ESP_LOGE(TAG, "Error while decoding: %d", error);
    this->jpeg_.close();
    switch (error) {
      case JPEG_ERROR_MEMORY:
        return DECODE_ERROR_OUT_OF_MEMORY;
      case JPEG_UNSUPPORTED_FEATURE:
        return DECODE_ERROR_UNSUPPORTED_FORMAT;
      case JPEG_INVALID_FILE:
      case JPEG_INVALID_PARAMETER:
        return DECODE_ERROR_INVALID_TYPE;
      case JPEG_DECODE_ERROR:
      default:
        return DECODE_ERROR_INTERNAL_DECODER_ERROR;
    }
  }
  this->decoded_bytes_ = size;
  this->jpeg_.close();
  return size;
}

#endif

}  // namespace esphome::runtime_image

#endif  // USE_RUNTIME_IMAGE_JPEG
