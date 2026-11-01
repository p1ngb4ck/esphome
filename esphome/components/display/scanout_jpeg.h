#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "display.h"

#include "driver/jpeg_decode.h"

#include <cstddef>
#include <cstdint>

namespace esphome::display {

bool decode_jpeg_into(jpeg_decoder_handle_t decoder, const uint8_t *jpeg, size_t len, uint8_t *target,
                      size_t capacity, uint8_t bpp);

class ScanoutJpeg {
 public:
  bool present(Display *display, jpeg_decoder_handle_t decoder, const uint8_t *jpeg, size_t len,
               uint32_t flip_wait_ms);
  bool decode(jpeg_decoder_handle_t decoder, const uint8_t *jpeg, size_t len, uint8_t *area, uint32_t width,
              uint32_t height, uint8_t bpp, uint8_t index, uint32_t layout);
  void reset() {
    this->key_ = 0;
    this->layout_ = 0;
    this->cleared_ = 0;
  }

 protected:
  uint32_t key_{0};
  uint32_t layout_{0};
  uint8_t cleared_{0};
};

}

#endif
