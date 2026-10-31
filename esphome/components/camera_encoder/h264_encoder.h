#pragma once

#include "esphome/core/defines.h"

#ifdef USE_CAMERA_H264_ENCODER

#include <atomic>

#include <esp_h264_enc_param.h>
#include <esp_h264_enc_single.h>

#include "esphome/components/camera/encoder.h"

namespace esphome::camera_encoder {

class H264Output final : public camera::EncoderBuffer {
 public:
  bool set_buffer_size(size_t size) override;
  uint8_t *get_data() const override { return this->data_; }
  size_t get_size() const override { return this->size_; }
  size_t get_max_size() const override { return this->capacity_; }
  bool allocate(size_t size);
  void release();

 protected:
  uint8_t *data_{nullptr};
  size_t capacity_{0};
  size_t size_{0};
};

class H264Encoder final : public camera::Encoder {
 public:
  H264Encoder(uint32_t bitrate, uint8_t gop, uint8_t fps, uint8_t qp_min, uint8_t qp_max)
      : bitrate_(bitrate), gop_(gop), fps_(fps), qp_min_(qp_min), qp_max_(qp_max) {}

  camera::EncoderError encode_pixels(camera::CameraImageSpec *spec, camera::Buffer *pixels) override;
  camera::EncoderBuffer *get_output_buffer() override { return &this->output_; }
  void dump_config() override;
  camera::ImageFormat get_format() const override { return camera::IMAGE_FORMAT_H264; }
  bool start(const camera::CameraImageSpec &spec) override;
  void stop() override;
  bool is_key_frame() const override { return this->key_frame_; }
  void request_key_frame() override { this->key_wanted_.store(true); }

  void set_bitrate(uint32_t bitrate);
  void set_gop(uint8_t gop);
  uint32_t get_bitrate() const { return this->bitrate_.load(); }
  uint8_t get_gop() const { return this->gop_.load(); }
  bool is_running() const { return this->handle_ != nullptr; }

 protected:
  std::atomic<uint32_t> bitrate_;
  std::atomic<uint8_t> gop_;
  uint8_t fps_;
  uint8_t qp_min_;
  uint8_t qp_max_;
  esp_h264_enc_handle_t handle_{nullptr};
  esp_h264_enc_param_handle_t param_{nullptr};
  H264Output output_;
  uint16_t width_{0};
  uint16_t height_{0};
  size_t frame_size_{0};
  uint32_t pts_{0};
  bool key_frame_{false};
  std::atomic<bool> key_wanted_{false};
  std::atomic<bool> rate_changed_{false};
};

}

#endif
