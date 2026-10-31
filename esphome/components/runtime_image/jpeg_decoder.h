#pragma once
#include "image_decoder.h"
#include "runtime_image.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
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
#if !defined(USE_HWJPG) && !defined(USE_NEWJPEG)
  JpegDecoder(RuntimeImage *image);

  int prepare(size_t expected_size) override;
#else
  JpegDecoder(RuntimeImage *image) : ImageDecoder(image, JPEG) {}
  ~JpegDecoder() override {}
#endif
  int HOT decode(uint8_t *buffer, size_t size) override;
  void reset() override;

#if !defined(USE_HWJPG) && !defined(USE_NEWJPEG)
  /// The source is read as the decode needs it, through JPEGDEC's own 2 KB buffer
  bool supports_source() const override { return true; }
  int begin_source(DecodeSource *source) override;
  /// Decodes one row of MCUs (8 or 16 pixel rows), or waits part way through one for more of the source
  int HOT decode_step() override;
#endif

#if !defined(USE_HWJPG) && !defined(USE_NEWJPEG)
  /// JPEGDEC decodes the image at 1/get_scale() of its size: 1, 2, 4 or 8
  int get_scale() const { return this->scale_; }
#endif
  void draw_rgb565(int x, int y, int w, int h, const uint8_t *pixels);

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
  /// Set up an opened image for decoding. Returns 0, or a DecodeError.
  int start_();
  /// The JPEGDEC option for scale_: JPEG_SCALE_HALF, _QUARTER and _EIGHTH are the divisors themselves
  int scale_option_() const { return this->scale_ == 1 ? 0 : this->scale_; }
  /// The DecodeError for a failed read of the source or JPEGDEC's last error, closing the image
  int fail_();

  static int32_t source_read(JPEGFILE *file, uint8_t *buf, int32_t len);
  static int32_t source_seek(JPEGFILE *file, int32_t position);
  static void source_close(void *handle) {}

  /// About 18 KB of decoder state: PSRAM first, nullptr if out of memory
  RAMUniquePtr<JPEGDEC> jpeg_;
  int scale_{1};                   // JPEGDEC decodes at 1/scale_ of the image size
  DecodeSource *source_{nullptr};  // Set while a begin_source() decode is in progress
#endif
  bool source_failed_{false};
};

}  // namespace esphome::runtime_image

#endif  // USE_RUNTIME_IMAGE_JPEG
