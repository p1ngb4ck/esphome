#include "scanout_jpeg.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esp_cache.h"

#include <cstring>

namespace esphome::display {

bool decode_jpeg_into(jpeg_decoder_handle_t decoder, const uint8_t *jpeg, size_t len, uint8_t *target,
                      size_t capacity, uint8_t bpp) {
  if (decoder == nullptr || jpeg == nullptr || target == nullptr)
    return false;
  jpeg_decode_cfg_t cfg{};
  cfg.output_format = bpp == 3 ? JPEG_DECODE_OUT_FORMAT_RGB888 : JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  uint32_t out = 0;
  return jpeg_decoder_process(decoder, &cfg, jpeg, static_cast<uint32_t>((len + 15) & ~static_cast<size_t>(15)),
                              target, static_cast<uint32_t>(capacity), &out) == ESP_OK &&
         out != 0;
}

bool ScanoutJpeg::present(Display *display, jpeg_decoder_handle_t decoder, const uint8_t *jpeg, size_t len,
                          uint32_t flip_wait_ms) {
  void *fbs[2]{};
  ColorBitness bitness{};
  if (display == nullptr || jpeg == nullptr || display->get_scanout_buffers(fbs, 2, &bitness) != 2)
    return false;
  uint8_t *target = display->scanout_back_buffer(flip_wait_ms);
  if (target == nullptr)
    return false;
  if (!this->decode(decoder, jpeg, len, target, static_cast<uint32_t>(display->get_native_width()),
                    static_cast<uint32_t>(display->get_native_height()), bitness == COLOR_BITNESS_888 ? 3 : 2,
                    target == fbs[0] ? 0 : 1, 0))
    return false;
  display->show_scanout_buffer(target);
  return true;
}

bool ScanoutJpeg::decode(jpeg_decoder_handle_t decoder, const uint8_t *jpeg, size_t len, uint8_t *area,
                         uint32_t width, uint32_t height, uint8_t bpp, uint8_t index, uint32_t layout) {
  jpeg_decode_picture_info_t info{};
  if (area == nullptr || jpeg == nullptr ||
      jpeg_decoder_get_info(jpeg, static_cast<uint32_t>(len), &info) != ESP_OK)
    return false;
  const uint32_t rows = (info.height + 15) & ~15u;
  if (info.width != width || rows > height)
    return false;
  const size_t stride = static_cast<size_t>(width) * bpp;
  const uint32_t key = (info.width << 16) | info.height;
  if (this->key_ != key || this->layout_ != layout) {
    this->key_ = key;
    this->layout_ = layout;
    this->cleared_ = 0;
  }
  if (rows < height && (this->cleared_ & (1u << index)) == 0) {
    std::memset(area, 0, stride * height);
    esp_cache_msync(area, stride * height, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    this->cleared_ |= static_cast<uint8_t>(1u << index);
  }
  const uint32_t dy = ((height - rows) / 2) & ~1u;
  return decode_jpeg_into(decoder, jpeg, len, area + static_cast<size_t>(dy) * stride, stride * (height - dy), bpp);
}

}

#endif
