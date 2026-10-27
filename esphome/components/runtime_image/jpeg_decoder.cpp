#include "jpeg_decoder.h"
#ifdef USE_RUNTIME_IMAGE_JPEG

#include "esphome/components/display/display_buffer.h"
#include "esphome/components/image/image.h"
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <new>

#ifdef USE_ESP_IDF
#include "esp_task_wdt.h"
#endif

#if defined(USE_HWJPG)
#include "esp_heap_caps.h"
#endif

ESPHOME_LOG_TAG(TAG, "image_decoder.jpeg");

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

void JpegDecoder::draw_rgb565(int x, int y, int w, int h, const uint8_t *pixels) {
  if (pixels == nullptr) {
    return;
  }
  uint8_t *const buffer = this->image_buffer_();
  const uint32_t buffer_width = this->image_->get_buffer_width();
  // JPEGDEC gives little-endian words; a big-endian image stores the high byte first
  const uint8_t first = this->image_is_big_endian_() ? 1 : 0;
  for (int source_y = 0; source_y < h; source_y++) {
    const int absolute_y = y + source_y;
    if (absolute_y < 0 || absolute_y >= this->draw_height_) {
      continue;
    }
    const uint16_t y1 = this->y_boundaries_[absolute_y];
    const uint16_t y2 = this->y_boundaries_[absolute_y + 1];
    for (int source_x = 0; source_x < w; source_x++) {
      const int absolute_x = x + source_x;
      if (absolute_x < 0 || absolute_x >= this->draw_width_) {
        continue;
      }
      const uint16_t x1 = this->x_boundaries_[absolute_x];
      const uint16_t x2 = this->x_boundaries_[absolute_x + 1];
      const uint8_t *pixel = pixels + (source_y * w + source_x) * 2;
      for (uint16_t destination_y = y1; destination_y < y2; destination_y++) {
        uint8_t *destination = buffer + (destination_y * buffer_width + x1) * 2;
        for (uint16_t destination_x = x1; destination_x < x2; destination_x++) {
          destination[0] = pixel[first];
          destination[1] = pixel[1 - first];
          destination += 2;
        }
      }
    }
  }
}

/**
 * @brief Callback method that will be called by the JPEGDEC engine when a chunk
 * of the image is decoded.
 *
 * @param jpeg  The JPEGDRAW object, including the context data.
 */
static int draw_callback(JPEGDRAW *jpeg) {
  JpegDecoder *decoder = (JpegDecoder *) jpeg->pUser;

  if (decoder == nullptr) {
    ESP_LOGE(TAG, "Decoder pointer is null!");
    return 0;
  }

  // Some very big images take too long to decode, so feed the watchdog on each callback
  // to avoid crashing if the executing task has a watchdog enabled.
#ifdef USE_ESP_IDF
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
#endif
    App.feed_wdt();
#ifdef USE_ESP_IDF
  }
#endif
  if (decoder->uses_direct_rgb565()) {
    decoder->draw_rgb565(jpeg->x, jpeg->y, jpeg->iWidth, jpeg->iHeight,
                         reinterpret_cast<const uint8_t *>(jpeg->pPixels));
    return 1;
  }
  size_t position = 0;
  size_t height = static_cast<size_t>(jpeg->iHeight);
  size_t width = static_cast<size_t>(jpeg->iWidth);
  for (size_t y = 0; y < height; y++) {
    for (size_t x = 0; x < width; x++) {
      auto rg = decode_value(jpeg->pPixels[position++]);
      auto ba = decode_value(jpeg->pPixels[position++]);
      Color color(rg[1], rg[0], ba[1], ba[0]);

      decoder->draw(jpeg->x + x, jpeg->y + y, 1, 1, color);
    }
  }
  return 1;
}

JpegDecoder::JpegDecoder(RuntimeImage *image) : ImageDecoder(image, JPEG) {
  RAMAllocator<JPEGDEC> allocator;
  JPEGDEC *jpeg = allocator.allocate(1);
  if (jpeg == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate memory for the JPEG decoder");
    return;
  }
  this->jpeg_ = new (jpeg) JPEGDEC();
}

JpegDecoder::~JpegDecoder() {
  if (this->jpeg_ != nullptr) {
    this->jpeg_->~JPEGDEC();
    RAMAllocator<JPEGDEC> allocator;
    allocator.deallocate(this->jpeg_, 1);
  }
}

int JpegDecoder::prepare(size_t expected_size) {
  // Check before the base prepare() so a failure never leaves an active session
  if (this->jpeg_ == nullptr) {
    ESP_LOGE(TAG, "JPEG decoder engine not initialized!");
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  return ImageDecoder::prepare(expected_size);
}

// Other ESP32 variants and host: JPEGDEC (bitbank2, software).
template<> int JpegDecoder::decode_backend_<JpegBackend::JPEGDEC>(uint8_t *buffer, size_t size) {
  if (this->jpeg_ == nullptr) {
    ESP_LOGE(TAG, "JPEG decoder engine not initialized!");
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  if (!this->jpeg_->openRAM(buffer, size, draw_callback)) {
    ESP_LOGE(TAG, "Could not open image for decoding: %d", this->jpeg_->getLastError());
    return DECODE_ERROR_INVALID_TYPE;
  }
  const int err = this->start_();
  if (err != 0)
    return err;
  if (!this->jpeg_->decode(0, 0, this->scale_option_()))
    return this->fail_();
  this->decoded_bytes_ = size;
  this->jpeg_->close();
  return size;
}

int JpegDecoder::start_() {
  auto jpeg_type = this->jpeg_->getJPEGType();
  if (jpeg_type == JPEG_MODE_INVALID) {
    ESP_LOGE(TAG, "Unsupported JPEG image");
    return DECODE_ERROR_INVALID_TYPE;
  } else if (jpeg_type == JPEG_MODE_PROGRESSIVE) {
    ESP_LOGE(TAG, "Progressive JPEG images not supported");
    return DECODE_ERROR_UNSUPPORTED_FORMAT;
  }
  ESP_LOGD(TAG, "Image size: %d x %d, bpp: %d", this->jpeg_->getWidth(), this->jpeg_->getHeight(),
           this->jpeg_->getBpp());

  this->jpeg_->setUserPointer(this);
  const int width = this->jpeg_->getWidth();
  const int height = this->jpeg_->getHeight();
  // The tables for drawing are only built once the decoded size is known, below
  if (!this->resize_image_(width, height)) {
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  // JPEGDEC can decode at 1/2, 1/4 or 1/8 size for much less work. Use the smallest size that still has at least as
  // many pixels as the buffer, so a large image shown small is not decoded in full only to be thrown away.
  const int buffer_width = this->image_->get_buffer_width();
  const int buffer_height = this->image_->get_buffer_height();
  this->scale_ = 1;
  while (this->scale_ < JPEG_SCALE_EIGHTH && width / (this->scale_ * 2) >= buffer_width &&
         height / (this->scale_ * 2) >= buffer_height) {
    this->scale_ *= 2;
  }
  const int decoded_width = (width + this->scale_ - 1) / this->scale_;
  const int decoded_height = (height + this->scale_ - 1) / this->scale_;
  if (this->scale_ != 1) {
    ESP_LOGD(TAG, "Decoding at 1/%d size, %d x %d", this->scale_, decoded_width, decoded_height);
  }
  this->prepare_draw_(decoded_width, decoded_height);
  if (this->direct_rgb565_) {
    // JPEGDEC's RGB565_BIG_ENDIAN output is not byte swapped on every build (the x86 host build
    // returns little-endian words for it), so the byte order is applied in draw_rgb565 instead
    this->jpeg_->setPixelType(RGB565_LITTLE_ENDIAN);
  } else {
    this->jpeg_->setPixelType(RGB8888);
  }
  return 0;
}

int JpegDecoder::fail_() {
  const bool read_failed = this->source_ != nullptr && this->source_failed_;
  const int error = this->jpeg_->getLastError();
  this->jpeg_->close();
  this->source_ = nullptr;
  if (read_failed) {
    ESP_LOGE(TAG, "Reading the image failed");
    return DECODE_ERROR_READ;
  }
  ESP_LOGE(TAG, "Error while decoding: %d", error);
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

void JpegDecoder::reset() {
  // A decode from a source that was given up part way still has the image open
  if (this->source_ != nullptr) {
    this->jpeg_->close();
    this->source_ = nullptr;
  }
  ImageDecoder::reset();
}

int32_t JpegDecoder::source_read(JPEGFILE *file, uint8_t *buf, int32_t len) {
  auto *decoder = static_cast<JpegDecoder *>(file->fHandle);
  const int32_t n = decoder->source_->read(buf, len);
  if (n < 0) {
    // JPEGDEC has no way to report a failed read, so remember it and give it no data
    decoder->source_failed_ = true;
    return 0;
  }
  file->iPos += n;
  return n;
}

int32_t JpegDecoder::source_seek(JPEGFILE *file, int32_t position) {
  auto *decoder = static_cast<JpegDecoder *>(file->fHandle);
  if (position < 0 || !decoder->source_->seek(position)) {
    decoder->source_failed_ = true;
    return file->iPos;
  }
  file->iPos = position;
  return position;
}

int JpegDecoder::begin_source(DecodeSource *source) {
  if (this->jpeg_ == nullptr) {
    ESP_LOGE(TAG, "JPEG decoder engine not initialized!");
    return DECODE_ERROR_OUT_OF_MEMORY;
  }
  if (this->expected_size_ == 0 || this->expected_size_ > INT32_MAX) {
    ESP_LOGE(TAG, "Image size %zu cannot be read from a source", this->expected_size_);
    return DECODE_ERROR_INVALID_TYPE;
  }
  if (!source->seek(0)) {
    ESP_LOGE(TAG, "Could not read the image");
    return DECODE_ERROR_READ;
  }
  this->source_ = source;
  this->source_failed_ = false;
  if (!this->jpeg_->open(this, this->expected_size_, source_close, source_read, source_seek, draw_callback)) {
    // The caller may recover, for example by reading the whole image first, so it reports the failure
    ESP_LOGD(TAG, "Could not open image for decoding: %d", this->jpeg_->getLastError());
    this->source_ = nullptr;
    return this->source_failed_ ? DECODE_ERROR_READ : DECODE_ERROR_INVALID_TYPE;
  }
  const int err = this->start_();
  if (err != 0) {
    this->jpeg_->close();
    this->source_ = nullptr;
    return err;
  }
  if (!this->jpeg_->decodeBegin(0, 0, this->scale_option_()))
    return this->fail_();
  return 0;
}

int HOT JpegDecoder::decode_step() {
  if (this->source_ == nullptr)
    return DECODE_ERROR_INVALID_TYPE;
  const int result = this->jpeg_->decodeRows(1);
  // JPEGDEC may decode on with the missing data, so check the read before its result
  if (this->source_failed_)
    return this->fail_();
  if (result == JPEG_DECODE_MORE)
    return DECODE_STEP_MORE;
  if (result == JPEG_DECODE_NEED_DATA)
    return DECODE_STEP_NEED_DATA;
  if (result == 0)
    return this->fail_();
  this->decoded_bytes_ = this->expected_size_;
  this->jpeg_->close();
  this->source_ = nullptr;
  return DECODE_STEP_DONE;
}

#endif

}  // namespace esphome::runtime_image

#endif  // USE_RUNTIME_IMAGE_JPEG
