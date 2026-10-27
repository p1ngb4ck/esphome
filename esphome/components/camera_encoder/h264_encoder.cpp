#include "esphome/core/defines.h"

#ifdef USE_CAMERA_H264_ENCODER

#include "h264_encoder.h"

#include <cinttypes>

#include <esp_h264_alloc.h>
#include <esp_h264_enc_param_hw.h>
#include <esp_h264_enc_single_hw.h>

#include "esphome/core/log.h"

namespace esphome::camera_encoder {

ESPHOME_LOG_TAG(TAG, "camera_encoder.h264");

static constexpr uint32_t OUTPUT_ALIGNMENT = 64;

bool H264Output::set_buffer_size(size_t size) {
  if (size > this->capacity_)
    return false;
  this->size_ = size;
  return true;
}

bool H264Output::allocate(size_t size) {
  this->release();
  uint32_t actual = 0;
  this->data_ = static_cast<uint8_t *>(
      esp_h264_aligned_calloc(OUTPUT_ALIGNMENT, 1, static_cast<uint32_t>(size), &actual, ESP_H264_MEM_SPIRAM));
  if (this->data_ == nullptr)
    return false;
  this->capacity_ = actual;
  this->size_ = 0;
  return true;
}

void H264Output::release() {
  if (this->data_ != nullptr)
    esp_h264_free(this->data_);
  this->data_ = nullptr;
  this->capacity_ = 0;
  this->size_ = 0;
}

static bool raw_format(camera::PixelFormat format, esp_h264_raw_format_t *out) {
  switch (format) {
    case camera::PIXEL_FORMAT_RGB565:
      *out = ESP_H264_RAW_FMT_RGB565_LE;
      return true;
    case camera::PIXEL_FORMAT_BGR888:
      *out = ESP_H264_RAW_FMT_BGR888;
      return true;
    case camera::PIXEL_FORMAT_YUYV:
      *out = ESP_H264_RAW_FMT_YUYV;
      return true;
    case camera::PIXEL_FORMAT_UYVY:
      *out = ESP_H264_RAW_FMT_UYVY;
      return true;
    case camera::PIXEL_FORMAT_GRAYSCALE:
      break;
  }
  return false;
}

bool H264Encoder::start(const camera::CameraImageSpec &spec) {
  if (this->handle_ != nullptr && spec.width == this->width_ && spec.height == this->height_)
    return true;
  this->stop();
  esp_h264_raw_format_t format;
  if (!raw_format(spec.format, &format)) {
    ESP_LOGE(TAG, "Pixel format %s cannot be encoded", camera::to_string(spec.format));
    return false;
  }
  if (spec.width % 16 != 0 || spec.height % 16 != 0) {
    ESP_LOGE(TAG, "%ux%u: width and height must be multiples of 16", spec.width, spec.height);
    return false;
  }
  esp_h264_enc_cfg_hw_t cfg{};
  cfg.pic_type = format;
  cfg.gop = this->gop_.load();
  cfg.fps = this->fps_;
  cfg.res.width = spec.width;
  cfg.res.height = spec.height;
  cfg.rc.bitrate = this->bitrate_.load();
  cfg.rc.qp_min = this->qp_min_;
  cfg.rc.qp_max = this->qp_max_;
  esp_h264_err_t err = esp_h264_enc_hw_new(&cfg, &this->handle_);
  if (err != ESP_H264_ERR_OK || this->handle_ == nullptr) {
    ESP_LOGE(TAG, "No encoder for %ux%u (error %d)", spec.width, spec.height, static_cast<int>(err));
    this->handle_ = nullptr;
    return false;
  }
  esp_h264_enc_param_hw_handle_t param = nullptr;
  if (esp_h264_enc_hw_get_param_hd(this->handle_, &param) != ESP_H264_ERR_OK || param == nullptr ||
      esp_h264_enc_open(this->handle_) != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "The encoder did not open");
    this->stop();
    return false;
  }
  this->param_ = &param->base;
  const size_t pixels = static_cast<size_t>(spec.width) * spec.height;
  if (!this->output_.allocate(pixels + pixels / 2)) {
    ESP_LOGE(TAG, "No PSRAM for the output (%zu bytes)", pixels + pixels / 2);
    this->stop();
    return false;
  }
  this->width_ = spec.width;
  this->height_ = spec.height;
  camera::CameraImageSpec copy = spec;
  this->frame_size_ = copy.bytes_per_image();
  this->pts_ = 0;
  this->key_frame_ = false;
  this->rate_changed_.store(false);
  ESP_LOGI(TAG, "Encoding %ux%u at %" PRIu32 " bit/s, GOP %u", spec.width, spec.height, cfg.rc.bitrate, cfg.gop);
  return true;
}

void H264Encoder::stop() {
  if (this->handle_ != nullptr) {
    esp_h264_enc_close(this->handle_);
    esp_h264_enc_del(this->handle_);
    this->handle_ = nullptr;
  }
  this->param_ = nullptr;
  this->output_.release();
  this->width_ = 0;
  this->height_ = 0;
  this->frame_size_ = 0;
}

camera::EncoderError H264Encoder::encode_pixels(camera::CameraImageSpec *spec, camera::Buffer *pixels) {
  if (this->handle_ == nullptr || spec->width != this->width_ || spec->height != this->height_ ||
      pixels->get_data_length() < this->frame_size_)
    return camera::ENCODER_ERROR_CONFIGURATION;
  if (this->rate_changed_.exchange(false)) {
    esp_h264_enc_set_bitrate(this->param_, this->bitrate_.load());
    esp_h264_enc_set_gop(this->param_, this->gop_.load());
  }
  if (this->key_wanted_.exchange(false))
    esp_h264_enc_force_idr(this->param_);
  esp_h264_enc_in_frame_t in{};
  in.raw_data.buffer = pixels->get_data_buffer();
  in.raw_data.len = static_cast<uint32_t>(this->frame_size_);
  in.pts = this->pts_;
  esp_h264_enc_out_frame_t out{};
  out.raw_data.buffer = this->output_.get_data();
  out.raw_data.len = static_cast<uint32_t>(this->output_.get_max_size());
  const esp_h264_err_t err = esp_h264_enc_process(this->handle_, &in, &out);
  this->pts_ += 90000 / (this->fps_ != 0 ? this->fps_ : 30);
  if (err != ESP_H264_ERR_OK || out.length == 0) {
    this->output_.set_buffer_size(0);
    return camera::ENCODER_ERROR_SKIP_FRAME;
  }
  this->output_.set_buffer_size(out.length);
  this->key_frame_ = out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I;
  return camera::ENCODER_ERROR_SUCCESS;
}

void H264Encoder::set_bitrate(uint32_t bitrate) {
  this->bitrate_.store(bitrate);
  this->rate_changed_.store(true);
}

void H264Encoder::set_gop(uint8_t gop) {
  this->gop_.store(gop);
  this->rate_changed_.store(true);
}

void H264Encoder::dump_config() {
  ESP_LOGCONFIG(TAG,
                "H.264 encoder (P4 hardware):\n"
                "  Bitrate: %" PRIu32 " bit/s\n"
                "  GOP: %u\n"
                "  FPS: %u\n"
                "  QP: %u..%u",
                this->bitrate_.load(), this->gop_.load(), this->fps_, this->qp_min_, this->qp_max_);
}

}

#endif
