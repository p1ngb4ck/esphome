#pragma once

#include "image_decoder.h"
#include "runtime_image.h"
#include "esphome/core/defines.h"
#ifdef USE_RUNTIME_IMAGE_JPEG

#if defined(USE_HWJPG)
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"
#elif defined(USE_NEWJPEG)
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#else
#include <JPEGDEC.h>
#endif

namespace esphome::runtime_image {

/// Which JPEG backend esp32.require_hw_jpeg() selected for this platform.
enum class JpegBackend {
  HW_P4,     // ESP32-P4 hardware JPEG codec (esp_driver_jpeg)
  NEW_JPEG,  // ESP32-S2/S3 esp_new_jpeg (SIMD-optimized software)
  JPEGDEC,   // Software fallback (bitbank2/JPEGDEC) - other ESP32 variants and host
};

#if defined(USE_HWJPG)
static constexpr JpegBackend JPEG_BACKEND = JpegBackend::HW_P4;
#elif defined(USE_NEWJPEG)
static constexpr JpegBackend JPEG_BACKEND = JpegBackend::NEW_JPEG;
#else
static constexpr JpegBackend JPEG_BACKEND = JpegBackend::JPEGDEC;
#endif

/**
 * @brief Image decoder specialization for JPEG images.
 */
class JpegDecoder : public ImageDecoder {
 public:
  /**
   * @brief Construct a new JPEG Decoder object.
   *
   * @param image The RuntimeImage to decode the stream into.
   */
  JpegDecoder(RuntimeImage *image) : ImageDecoder(image, JPEG) {}
  ~JpegDecoder() override {}

  int HOT decode(uint8_t *buffer, size_t size) override;

 protected:
  // Backend-specific decode. JPEG_BACKEND selects which explicit
  // specialization is instantiated at the call site in decode(); only one
  // specialization is ever defined in jpeg_decoder.cpp for a given build
  // (each lives behind the same #ifdef that guards its backend's headers
  // above), so the constexpr is what gates which codec API actually runs.
  template<JpegBackend Backend> int decode_backend_(uint8_t *buffer, size_t size);

#if defined(USE_HWJPG) || defined(USE_NEWJPEG)
  // Hardware/SIMD backends decode into one contiguous RGB888 buffer; this
  // walks it pixel-by-pixel into the RuntimeImage via draw().
  void draw_rgb888_buffer_(const uint8_t *rgb, uint32_t width, uint32_t height);
#else
  JPEGDEC jpeg_{};
#endif
};

}  // namespace esphome::runtime_image

#endif  // USE_RUNTIME_IMAGE_JPEG
