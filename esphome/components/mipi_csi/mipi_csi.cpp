#include "mipi_csi.h"

#include <esp_log.h>

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <unistd.h>

#include <driver/i2c_master.h>
#include <esp_cam_sensor_xclk.h>
#include <esp_video_device.h>
#include <esp_video_init.h>
#include <esp_video_ioctl.h>
#include <linux/videodev2.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::mipi_csi {

ESPHOME_LOG_TAG(TAG, "mipi_csi");

static constexpr uint32_t CAPTURE_TASK_STACK_SIZE = 4096;
static constexpr UBaseType_t CAPTURE_TASK_PRIORITY = 1;
static constexpr BaseType_t CAPTURE_TASK_CORE = 1;
static constexpr uint32_t JPEG_ENGINE_TIMEOUT_MS = 200;

/// How a configured pixel format maps onto the video device and the JPEG encoder.
struct FormatMapping {
  uint32_t fourcc;
  jpeg_enc_input_format_t jpeg_input;
  jpeg_down_sampling_type_t sub_sample;
  uint8_t bytes_per_pixel;
  const char *name;
};

static FormatMapping get_format_mapping(PixelFormat format) {
  switch (format) {
    case PixelFormat::PIXEL_FORMAT_RGB888:
      return {V4L2_PIX_FMT_RGB24, JPEG_ENCODE_IN_FORMAT_RGB888, JPEG_DOWN_SAMPLING_YUV444, 3, "RGB888"};
    case PixelFormat::PIXEL_FORMAT_YUV422:
      return {V4L2_PIX_FMT_UYVY, JPEG_ENCODE_IN_FORMAT_YUV422, JPEG_DOWN_SAMPLING_YUV422, 2, "YUV422"};
    case PixelFormat::PIXEL_FORMAT_GRAYSCALE:
      return {V4L2_PIX_FMT_GREY, JPEG_ENCODE_IN_FORMAT_GRAY, JPEG_DOWN_SAMPLING_GRAY, 1, "GRAYSCALE"};
    case PixelFormat::PIXEL_FORMAT_RGB565:
    default:
      return {V4L2_PIX_FMT_RGB565, JPEG_ENCODE_IN_FORMAT_RGB565, JPEG_DOWN_SAMPLING_YUV422, 2, "RGB565"};
  }
}

/// Holds the four characters of a fourcc code plus a terminator, for logging.
using FourccName = char[5];

static void format_fourcc(uint32_t fourcc, FourccName out) {
  out[0] = static_cast<char>(fourcc & 0xFF);
  out[1] = static_cast<char>((fourcc >> 8) & 0xFF);
  out[2] = static_cast<char>((fourcc >> 16) & 0xFF);
  out[3] = static_cast<char>((fourcc >> 24) & 0xFF);
  out[4] = '\0';
}

/* ---------------- MipiCsiCamera: setup ---------------- */

static uint8_t *psram_alloc(size_t size) {
  return static_cast<uint8_t *>(heap_caps_aligned_alloc(128, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void psram_free(uint8_t *ptr) { heap_caps_free(ptr); }

void MipiCsiCamera::setup() {
  if (this->alloc_ == nullptr || this->free_ == nullptr) {
    this->alloc_ = psram_alloc;
    this->free_ = psram_free;
  }
  if (!this->start_external_clock_()) {
    this->mark_failed();
    return;
  }
  // Nothing more until an image or a stream is asked for (start_capture_)
  this->disable_loop();
}

bool MipiCsiCamera::start_external_clock_() {
  if (this->xclk_pin_ < 0)
    return true;

#if CONFIG_CAMERA_XCLK_USE_ESP_CLOCK_ROUTER
  esp_cam_sensor_xclk_handle_t handle = nullptr;
  esp_err_t err = esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Allocating the sensor clock failed: %s", esp_err_to_name(err));
    return false;
  }

  esp_cam_sensor_xclk_config_t config{};
  config.esp_clock_router_cfg.xclk_pin = static_cast<gpio_num_t>(this->xclk_pin_);
  config.esp_clock_router_cfg.xclk_freq_hz = this->xclk_frequency_;
  err = esp_cam_sensor_xclk_start(handle, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Starting the sensor clock failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
#else
  ESP_LOGE(TAG, "Sensor clock output is not available in this build");
  return false;
#endif
}

bool MipiCsiCamera::init_video_() {
  // The ISP's algorithms (esp_ipa, a prebuilt library) log every frame at debug level: warnings and errors only
  static const char *const IPA_TAGS[] = {"esp_ipa_acc", "esp_ipa_adn", "esp_ipa_aen", "esp_ipa_af", "esp_ipa_agc",
                                         "esp_ipa_atc", "esp_ipa_awb", "esp_ipa_ext", "esp_ipa_ian"};
  for (const char *tag : IPA_TAGS)
    esp_log_level_set(tag, ESP_LOG_WARN);
  i2c_master_bus_handle_t i2c_handle = nullptr;
  esp_err_t err = i2c_master_get_bus_handle(this->i2c_bus_->get_port(), &i2c_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not use I2C port %d for the sensor: %s", this->i2c_bus_->get_port(), esp_err_to_name(err));
    return false;
  }

  esp_video_init_csi_config_t csi_config{};
  csi_config.sccb_config.init_sccb = false;
  csi_config.sccb_config.i2c_handle = i2c_handle;
  csi_config.sccb_config.freq = this->sccb_frequency_;
  csi_config.reset_pin = static_cast<gpio_num_t>(this->reset_pin_);
  csi_config.pwdn_pin = static_cast<gpio_num_t>(this->power_pin_);
  // When another peripheral already powers the D-PHY rail, claiming it a second time fails and
  // takes the whole camera down with it, so in that case it is left alone.
  csi_config.dont_init_ldo = !this->init_ldo_;

  esp_video_init_config_t video_config{};
  video_config.csi = &csi_config;

  // The ISP is always initialized as well: esp_video routes RAW sensors through it and starts the
  // IPA pipeline itself, and simply bypasses it for sensors that already output RGB or YUV.
  err = esp_video_init_with_flags(&video_config, ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Camera sensor initialization failed: %s", esp_err_to_name(err));
    if (this->init_ldo_) {
      ESP_LOGE(TAG, "If another peripheral already powers the camera's supply rail, set 'init_ldo: false'");
    }
    return false;
  }
  return true;
}

bool MipiCsiCamera::select_sensor_format_() {
  esp_cam_sensor_format_t chosen{};

  if (this->width_ == 0) {
    // No resolution configured, so keep the format the sensor driver starts up with.
    if (ioctl(this->fd_, VIDIOC_G_SENSOR_FMT, &chosen) != 0) {
      ESP_LOGE(TAG, "Reading the sensor's current format failed: %s", strerror(errno));
      return false;
    }
  } else {
    bool found = false;
    v4l2_sensor_format_enum enumerator{};
    for (enumerator.index = 0; ioctl(this->fd_, VIDIOC_ENUM_SENSOR_FMT, &enumerator) == 0; enumerator.index++) {
      const esp_cam_sensor_format_t &candidate = enumerator.format;
      if (candidate.port != ESP_CAM_SENSOR_MIPI_CSI || candidate.width != this->width_ ||
          candidate.height != this->height_) {
        continue;
      }
      if (!found || this->is_better_sensor_format_(candidate, chosen)) {
        chosen = candidate;
        found = true;
      }
    }

    if (!found) {
      ESP_LOGE(TAG, "The sensor cannot capture %ux%u. Supported resolutions are:", this->width_, this->height_);
      this->log_sensor_formats_();
      return false;
    }

    // The resolution belongs to the sensor, so it has to be selected here. VIDIOC_S_FMT only picks
    // the pixel format and rejects every resolution other than the sensor's current one.
    if (ioctl(this->fd_, VIDIOC_S_SENSOR_FMT, &chosen) != 0) {
      ESP_LOGE(TAG, "Switching the sensor to %ux%u failed: %s", this->width_, this->height_, strerror(errno));
      return false;
    }
  }

  this->width_ = chosen.width;
  this->height_ = chosen.height;
  this->framerate_ = this->resolve_framerate_(chosen.fps);
  ESP_LOGD(TAG, "Sensor format '%s': %ux%u at %u fps, %s",
           chosen.name != nullptr ? chosen.name : LOG_STR_LITERAL("unnamed"), chosen.width, chosen.height, chosen.fps,
           chosen.isp_info != nullptr ? LOG_STR_LITERAL("processed by the ISP")
                                      : LOG_STR_LITERAL("used as the sensor delivers it"));
  return true;
}

void MipiCsiCamera::log_sensor_formats_() {
  v4l2_sensor_format_enum enumerator{};
  for (enumerator.index = 0; ioctl(this->fd_, VIDIOC_ENUM_SENSOR_FMT, &enumerator) == 0; enumerator.index++) {
    const esp_cam_sensor_format_t &format = enumerator.format;
    if (format.port != ESP_CAM_SENSOR_MIPI_CSI)
      continue;
    ESP_LOGE(TAG, "  %ux%u at %u fps (%s)", format.width, format.height, format.fps,
             format.name != nullptr ? format.name : LOG_STR_LITERAL("unnamed"));
  }
}

bool MipiCsiCamera::is_better_sensor_format_(const esp_cam_sensor_format_t &candidate,
                                             const esp_cam_sensor_format_t &current) const {
  // Formats that carry ISP information are the RAW ones, and only those let the ISP run the image
  // tuning algorithms, so they are always preferred.
  if ((candidate.isp_info != nullptr) != (current.isp_info != nullptr))
    return candidate.isp_info != nullptr;

  // Prefer a sensor rate the configured rate divides into, because that is the only way the driver
  // can reach the configured rate exactly.
  const bool candidate_fits = candidate.fps != 0 && candidate.fps % this->framerate_ == 0;
  const bool current_fits = current.fps != 0 && current.fps % this->framerate_ == 0;
  if (candidate_fits != current_fits)
    return candidate_fits;

  return false;
}

uint8_t MipiCsiCamera::resolve_framerate_(uint8_t sensor_fps) const {
  if (sensor_fps == 0)
    return this->framerate_;

  // The driver slows the stream down by dropping whole frames, so it can only divide the sensor's
  // rate. Step down to the closest rate that divides it evenly.
  for (uint8_t fps = std::min<uint8_t>(this->framerate_, sensor_fps); fps >= 1; fps--) {
    if (sensor_fps % fps == 0) {
      if (fps != this->framerate_) {
        ESP_LOGW(TAG, "The sensor runs at %u fps, so %u fps is used instead of %u fps", sensor_fps, fps,
                 this->framerate_);
      }
      return fps;
    }
  }
  return sensor_fps;
}

bool MipiCsiCamera::configure_device_() {
  this->fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (this->fd_ < 0) {
    ESP_LOGE(TAG, "Opening %s failed: %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, strerror(errno));
    return false;
  }

  if (!this->select_sensor_format_())
    return false;

  const FormatMapping mapping = get_format_mapping(this->pixel_format_);

  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = this->width_;
  format.fmt.pix.height = this->height_;
  format.fmt.pix.pixelformat = mapping.fourcc;
  if (ioctl(this->fd_, VIDIOC_S_FMT, &format) != 0) {
    ESP_LOGE(TAG, "The camera cannot deliver %s at %ux%u: %s", mapping.name, this->width_, this->height_,
             strerror(errno));
    return false;
  }

  // Must come after the format has been set, and not every sensor offers a choice of frame rates.
  v4l2_streamparm stream_param{};
  stream_param.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  stream_param.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
  stream_param.parm.capture.timeperframe.numerator = 1;
  stream_param.parm.capture.timeperframe.denominator = this->framerate_;
  if (ioctl(this->fd_, VIDIOC_S_PARM, &stream_param) != 0) {
    ESP_LOGW(TAG, "The sensor kept its own frame rate: %s", strerror(errno));
  }

  if (this->horizontal_flip_.has_value())
    this->apply_control_(V4L2_CID_HFLIP, *this->horizontal_flip_, "horizontal flip");
  if (this->vertical_flip_.has_value())
    this->apply_control_(V4L2_CID_VFLIP, *this->vertical_flip_, "vertical flip");
  for (uint8_t i = 0; i < this->control_count_; i++)
    this->apply_control_(this->controls_[i].id, this->controls_[i].value, "an image control");

  if (!this->read_back_format_(mapping.fourcc, mapping.bytes_per_pixel, mapping.name))
    return false;

  return true;
}

bool MipiCsiCamera::read_back_format_(uint32_t expected_fourcc, uint8_t bytes_per_pixel, const char *name) {
  v4l2_format actual{};
  actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->fd_, VIDIOC_G_FMT, &actual) != 0) {
    ESP_LOGE(TAG, "Reading back the capture format failed: %s", strerror(errno));
    return false;
  }

  // The driver is free to settle on something other than what was asked for, and the encoder has to
  // be set up for what actually comes out, not for what was requested. Only the geometry and the
  // pixel format are meaningful here: esp_video hands back the very struct it was given, so its
  // bytesperline and sizeimage fields stay at whatever the caller left them.
  FourccName fourcc_name;
  format_fourcc(actual.fmt.pix.pixelformat, fourcc_name);
  ESP_LOGD(TAG, "Capture format: %" PRIu32 "x%" PRIu32 " '%s'", actual.fmt.pix.width, actual.fmt.pix.height,
           fourcc_name);

  // A different pixel format means every byte of the frame would be read with the wrong layout, so
  // the encoder would turn out a corrupted picture rather than a wrong-looking one.
  if (actual.fmt.pix.pixelformat != expected_fourcc) {
    FourccName expected_name;
    format_fourcc(expected_fourcc, expected_name);
    ESP_LOGE(TAG, "The camera delivers '%s' instead of the configured %s ('%s'); pick another 'pixel_format'",
             fourcc_name, name, expected_name);
    return false;
  }

  this->width_ = actual.fmt.pix.width;
  this->height_ = actual.fmt.pix.height;
  this->expected_frame_size_ = static_cast<size_t>(this->width_) * this->height_ * bytes_per_pixel;
  return true;
}

void MipiCsiCamera::apply_control_(uint32_t id, int32_t value, const char *name) {
  v4l2_ext_control control{};
  control.id = id;
  control.value = value;

  v4l2_ext_controls controls{};
  controls.ctrl_class = V4L2_CTRL_CLASS_USER;
  controls.count = 1;
  controls.controls = &control;

  if (ioctl(this->fd_, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
    ESP_LOGW(TAG, "The sensor does not support %s", name);
  }
}

bool MipiCsiCamera::set_control(uint32_t id, int32_t value) {
  Control *slot = nullptr;
  for (uint8_t i = 0; i < this->control_count_; i++) {
    if (this->controls_[i].id == id)
      slot = &this->controls_[i];
  }
  if (slot == nullptr) {
    if (this->control_count_ >= MAX_CONTROLS)
      return false;
    slot = &this->controls_[this->control_count_++];
    slot->id = id;
  }
  slot->value = value;
  if (this->fd_ >= 0)
    this->apply_control_(id, value, "an image control");
  return true;
}

void MipiCsiCamera::set_mirror(bool on) { this->set_control(V4L2_CID_HFLIP, on ? 1 : 0); }

void MipiCsiCamera::set_upside_down(bool on) { this->set_control(V4L2_CID_VFLIP, on ? 1 : 0); }

static size_t align_piece(size_t size) { return (size + 127) & ~static_cast<size_t>(127); }

bool MipiCsiCamera::open_jpeg_() {
  if (this->encoder_.is_open())
    return true;
  jpeg_encoder_handle_t engine = this->jpeg_engine_ != nullptr ? this->jpeg_engine_() : nullptr;
  if (this->jpeg_engine_ == nullptr) {
    jpeg_encode_engine_cfg_t cfg{};
    cfg.timeout_ms = JPEG_ENGINE_TIMEOUT_MS;
    if (jpeg_new_encoder_engine(&cfg, &this->own_jpeg_engine_) != ESP_OK)
      this->own_jpeg_engine_ = nullptr;
    engine = this->own_jpeg_engine_;
  }
  if (engine == nullptr) {
    ESP_LOGE(TAG, "No JPEG encoder engine");
    return false;
  }
  const size_t jpeg_size = align_piece(JpegEncoder::output_size_for(this->expected_frame_size_));
  this->jpeg_output_ = this->alloc_(jpeg_size);
  if (this->jpeg_output_ == nullptr) {
    ESP_LOGE(TAG, "No room for the JPEG output (%zu bytes)", jpeg_size);
    this->release_own_jpeg_engine_();
    return false;
  }
  const FormatMapping mapping = get_format_mapping(this->pixel_format_);
  this->encoder_.open(engine, this->width_, this->height_, mapping.jpeg_input, mapping.sub_sample, this->jpeg_quality_,
                      this->jpeg_output_, jpeg_size);
  return true;
}

void MipiCsiCamera::close_jpeg_() {
  this->encoder_.close();
  if (this->jpeg_output_ != nullptr) {
    this->free_(this->jpeg_output_);
    this->jpeg_output_ = nullptr;
  }
  this->release_own_jpeg_engine_();
}

void MipiCsiCamera::release_own_jpeg_engine_() {
  if (this->own_jpeg_engine_ == nullptr)
    return;
  jpeg_del_encoder_engine(this->own_jpeg_engine_);
  this->own_jpeg_engine_ = nullptr;
}

bool MipiCsiCamera::queue_buffer_(uint32_t index) {
  v4l2_buffer buffer{};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_USERPTR;
  buffer.index = index;
  buffer.m.userptr = reinterpret_cast<unsigned long>(this->buffers_[index].data);
  buffer.length = this->buffers_[index].length;
  return ioctl(this->fd_, VIDIOC_QBUF, &buffer) == 0;
}

bool MipiCsiCamera::start_capture_() {
  if (!this->init_video_()) {
    this->finish_stop_();
    return false;
  }
  this->video_ready_ = true;
  if (!this->configure_device_()) {
    this->finish_stop_();
    return false;
  }

  // The frames (one of them the driver's reserve: its own backup buffer is off), all from the allocator
  const size_t frame_piece = align_piece(this->expected_frame_size_);
  if (!this->buffers_.try_init(this->frame_buffer_count_)) {
    this->finish_stop_();
    return false;
  }
  for (uint8_t i = 0; i < this->frame_buffer_count_; i++) {
    uint8_t *data = this->alloc_(frame_piece);
    if (data == nullptr) {
      ESP_LOGE(TAG, "No room for frame buffer %u (%zu bytes)", i, frame_piece);
      this->finish_stop_();
      return false;
    }
    this->buffers_.push_back({data, frame_piece});
  }
  v4l2_requestbuffers request{};
  request.count = this->frame_buffer_count_;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_USERPTR;
  if (ioctl(this->fd_, VIDIOC_REQBUFS, &request) != 0 || request.count != this->frame_buffer_count_) {
    ESP_LOGE(TAG, "Requesting %u frame buffers failed: %s", this->frame_buffer_count_, strerror(errno));
    this->finish_stop_();
    return false;
  }
  for (uint32_t i = 0; i < request.count; i++) {
    if (!this->queue_buffer_(i)) {
      ESP_LOGE(TAG, "Queueing frame buffer %" PRIu32 " failed: %s", i, strerror(errno));
      this->finish_stop_();
      return false;
    }
  }

  this->settle_count_ = 0;
  this->settle_level_ = -1;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->fd_, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "Starting the video stream failed: %s", strerror(errno));
    this->finish_stop_();
    return false;
  }

  // The capture task: its stack from the allocator, its TCB internal (FreeRTOS needs it there); both only while
  // streaming
  this->capture_tcb_ =
      static_cast<StaticTask_t *>(heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  this->capture_stack_ = this->alloc_(CAPTURE_TASK_STACK_SIZE);
  if (this->capture_tcb_ != nullptr && this->capture_stack_ != nullptr) {
    this->capture_task_handle_ = xTaskCreateStaticPinnedToCore(
        &MipiCsiCamera::capture_task, "mipi_csi", CAPTURE_TASK_STACK_SIZE, this, CAPTURE_TASK_PRIORITY,
        reinterpret_cast<StackType_t *>(this->capture_stack_), this->capture_tcb_, CAPTURE_TASK_CORE);
  }
  if (this->capture_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "No room for the capture task");
    this->finish_stop_();
    return false;
  }
  this->state_ = State::RUNNING;
  ESP_LOGD(TAG, "Streaming: %u frames of %zu bytes", this->frame_buffer_count_, frame_piece);
  return true;
}

void MipiCsiCamera::finish_stop_() {
  if (this->direct_given_) {
    this->direct_given_ = false;
    this->direct_callback_.call(nullptr, 0, 0);
  }
  this->direct_held_.store(-1);
  this->direct_returned_.store(-1);
  this->direct_ready_.store(false);
  // The task has parked itself (suspended, nothing left to do): deleting it is safe; its stack and TCB go after
  if (this->capture_task_handle_ != nullptr) {
    vTaskDelete(this->capture_task_handle_);
    this->capture_task_handle_ = nullptr;
  }
  if (this->fd_ >= 0) {
    // Ignored if the stream was never turned on
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(this->fd_, VIDIOC_STREAMOFF, &type);
    v4l2_requestbuffers release{};
    release.count = 0;
    release.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    release.memory = V4L2_MEMORY_USERPTR;
    ioctl(this->fd_, VIDIOC_REQBUFS, &release);
    close(this->fd_);
    this->fd_ = -1;
  }
  this->close_jpeg_();
  this->h264_on_.store(false);
  this->h264_ready_.store(false);
  if (this->h264_started_ && this->h264_free_.load()) {
    this->h264_encoder_->stop();
    this->h264_started_ = false;
  }
  if (this->capture_tcb_ != nullptr) {
    heap_caps_free(this->capture_tcb_);
    this->capture_tcb_ = nullptr;
  }
  if (this->capture_stack_ != nullptr) {
    this->free_(this->capture_stack_);
    this->capture_stack_ = nullptr;
  }
  for (auto &buffer : this->buffers_) {
    this->free_(buffer.data);
  }
  this->buffers_.release();
  if (this->video_ready_) {
    esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP);
    this->video_ready_ = false;
  }
  this->current_image_.reset();
  if (this->preview_released_ != nullptr) {
    uint8_t *released = this->preview_released_;
    this->preview_released_ = nullptr;
    this->preview_released_callback_.call(released);
  }
  this->preview_wanted_.store(false);
  this->preview_ready_.store(false);
  this->frame_wanted_.store(false);
  this->result_ready_.store(false);
  this->stop_wanted_.store(false);
  this->parked_.store(false);
  this->failed_.store(false);
  this->state_ = State::IDLE;
}

static const char *flip_state(const optional<bool> &flip) {
  if (!flip.has_value())
    return LOG_STR_LITERAL("default");
  return *flip ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off");
}

void MipiCsiCamera::dump_config() {
  ESP_LOGCONFIG(TAG,
                "MIPI-CSI camera '%s':\n"
                "  Sensor: %s\n"
                "  Resolution: %ux%u\n"
                "  Capture format: %s\n"
                "  JPEG quality: %u\n"
                "  Frame rate: %u fps\n"
                "  Frame buffers: %u\n"
                "  Flip: horizontal %s, vertical %s",
                LOG_STR_ARG(this->get_log_name()), this->sensor_name_, this->width_, this->height_,
                get_format_mapping(this->pixel_format_).name, this->jpeg_quality_, this->framerate_,
                this->frame_buffer_count_, flip_state(this->horizontal_flip_), flip_state(this->vertical_flip_));
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Setup failed");
  }
}

/* ---------------- MipiCsiCamera: capture ---------------- */

void MipiCsiCamera::capture_task(void *param) {
  auto *self = static_cast<MipiCsiCamera *>(param);

  // The stream runs while anything is asked for, so that the ISP's auto exposure and white balance stay
  // converged. Frames nobody asked for go straight back to the driver.
  while (!self->stop_wanted_.load()) {
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(self->fd_, VIDIOC_DQBUF, &buffer) != 0) {
      ESP_LOGW(TAG, "Dequeueing a frame failed: %s", strerror(errno));
      self->failed_.store(true);
      break;
    }
    const int8_t returned = self->direct_returned_.exchange(-1);
    if (returned >= 0 && !self->queue_buffer_(static_cast<uint32_t>(returned))) {
      ESP_LOGW(TAG, "Returning a frame buffer failed: %s", strerror(errno));
    }

    const bool settled = (buffer.flags & V4L2_BUF_FLAG_ERROR) == 0 && self->settled_(buffer.index);
    if (self->frame_wanted_.load() && settled) {
      // The encoder is handed the whole buffer, as Espressif's reference does: the length is what
      // bounds the cache invalidation before the JPEG engine's DMA reads it.
      const FrameBuffer &frame = self->buffers_[buffer.index];
      // 0 tells the main loop that the request was not served, so a still image request is not dropped
      self->result_length_.store(self->encoder_.encode(frame.data, frame.length));
      self->result_ready_.store(true);
      // Cleared last, so that an unset flag always means the result is already there
      self->frame_wanted_.store(false);
      self->enable_loop_soon_any_context();
      App.wake_loop_threadsafe();
    }

    if (settled && self->h264_on_.load() && self->h264_free_.load())
      self->encode_h264_(self->buffers_[buffer.index]);

    // Busy first, then the buffer read: a buffer ended meanwhile is either seen as gone or waited for (loop)
    self->preview_busy_.store(true);
    uint8_t *target = self->preview_buffer_.load();
    if (target != nullptr && self->preview_wanted_.load() && (buffer.flags & V4L2_BUF_FLAG_ERROR) == 0) {
      const FrameBuffer &frame = self->buffers_[buffer.index];
      // The frame came by DMA: the cache lines over it are dropped before the CPU reads it
      esp_cache_msync(frame.data, frame.length, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
      memcpy(target, frame.data, std::min(self->preview_size_, self->expected_frame_size_));
      self->preview_wanted_.store(false);
      self->preview_ready_.store(true);
    }
    self->preview_busy_.store(false);
    if (self->preview_ready_.load() || self->preview_released_ != nullptr) {
      self->enable_loop_soon_any_context();
      App.wake_loop_threadsafe();
    }

    // Direct: this buffer is the reader's until it comes back (direct_returned_), the driver has the others; one
    // returned and not re-queued yet goes first (two returns between frames would lose one)
    if (self->direct_.load() && self->direct_held_.load() < 0 && self->direct_returned_.load() < 0 &&
        (buffer.flags & V4L2_BUF_FLAG_ERROR) == 0) {
      self->direct_held_.store(static_cast<int8_t>(buffer.index));
      self->direct_ready_.store(true);
      self->enable_loop_soon_any_context();
      App.wake_loop_threadsafe();
      continue;
    }

    if (!self->queue_buffer_(buffer.index)) {
      ESP_LOGW(TAG, "Returning a frame buffer failed: %s", strerror(errno));
    }
  }
  // Parked: the main loop deletes this task and gives back its stack, the buffers and esp_video
  self->parked_.store(true);
  self->enable_loop_soon_any_context();
  App.wake_loop_threadsafe();
  vTaskSuspend(nullptr);
}

void MipiCsiCamera::loop() {
  // Event driven: woken by a request, a finished frame, an image given back or the task parking
  if (this->state_ == State::IDLE) {
    if (!this->has_requested_image_()) {
      this->disable_loop();
      return;
    }
    if (!this->start_capture_()) {
      ESP_LOGE(TAG, "The camera could not start; the requests are dropped");
      this->single_requesters_ = 0;
      this->stream_requesters_ = 0;
      this->disable_loop();
      return;
    }
  }

  if (this->state_ == State::STOPPING || this->parked_.load()) {
    if (!this->parked_.load()) {
      this->disable_loop();  // the task wakes the loop once it has parked
      return;
    }
    const bool failed = this->failed_.load();
    this->finish_stop_();
    if (failed) {
      ESP_LOGE(TAG, "The stream failed; the requests are dropped");
      this->single_requesters_ = 0;
      this->stream_requesters_ = 0;
    }
    if (!this->has_requested_image_())
      this->disable_loop();
    return;
  }

  if (this->preview_released_ != nullptr && !this->preview_busy_.load()) {
    uint8_t *released = this->preview_released_;
    this->preview_released_ = nullptr;
    this->preview_released_callback_.call(released);
  }
  if (this->preview_ready_.load()) {
    this->preview_ready_.store(false);
    if (this->preview_buffer_.load() != nullptr)
      this->preview_callback_.call();
  }
  if (this->preview_buffer_.load() != nullptr && !this->preview_wanted_.load())
    this->preview_wanted_.store(true);
  if (this->direct_ready_.exchange(false)) {
    const int8_t held = this->direct_held_.load();
    if (held >= 0 && this->direct_.load()) {
      this->direct_given_ = true;
      this->direct_callback_.call(this->buffers_[held].data, this->width_, this->height_);
    } else {
      this->release_direct_frame();
    }
  }

  this->h264_step_();

  // Release the previous image once every consumer has finished reading it, which frees the
  // encoder's output buffer for the next frame.
  if (this->current_image_ && this->current_image_.use_count() == 1) {
    this->current_image_.reset();
  }

  if (!this->current_image_ && this->result_ready_.load()) {
    this->result_ready_.store(false);
    const size_t length = this->result_length_.load();
    if (length == 0) {
      // Encoding failed. Hand the one-shot requests back so the clients waiting for a still image
      // are served by the next frame instead of waiting forever. Streams ask again on their own.
      this->single_requesters_ |= static_cast<uint8_t>(this->pending_requesters_ & ~this->stream_requesters_.load());
      this->pending_requesters_ = 0;
    } else {
      this->current_image_ =
          std::make_shared<MipiCsiImage>(this->encoder_.get_output_buffer(), length, this->pending_requesters_);
      this->pending_requesters_ = 0;
      for (auto *listener : this->listeners_) {
        listener->on_camera_image(this->current_image_);
      }
      // Nobody kept it (no reader took it): free for the next frame at once
      if (this->current_image_.use_count() == 1)
        this->current_image_.reset();
    }
  }

  // An image still being read, or a frame on its way: the reader / the task wakes the loop again
  if (this->current_image_ || this->frame_wanted_.load()) {
    this->disable_loop();
    return;
  }
  if ((this->preview_buffer_.load() != nullptr || this->direct_.load() || this->h264_started_) &&
      this->single_requesters_ == 0 && this->stream_requesters_ == 0) {
    this->close_jpeg_();   // no API image wanted: the JPEG output back to the pool
    this->disable_loop();  // only the preview / direct frames: the task wakes the loop with each frame
    return;
  }
  if (!this->has_requested_image_()) {
    // Nothing asked for: the task stops taking frames and parks, then everything is given back
    this->state_ = State::STOPPING;
    this->stop_wanted_.store(true);
    this->disable_loop();
    return;
  }
  if (!this->open_jpeg_()) {
    ESP_LOGE(TAG, "No JPEG for the image requests; they are dropped");
    this->single_requesters_ = 0;
    this->stream_requesters_ = 0;
    this->enable_loop();  // the stop (or the preview) follows on the next pass
    return;
  }
  this->pending_requesters_ = this->single_requesters_ | this->stream_requesters_;
  this->single_requesters_ = 0;
  this->frame_wanted_.store(true);
  this->disable_loop();
}

/* ---------------- MipiCsiCamera: requests ---------------- */

uint8_t MipiCsiCamera::bytes_per_pixel_() const { return get_format_mapping(this->pixel_format_).bytes_per_pixel; }

int32_t MipiCsiCamera::frame_level_(const FrameBuffer &frame) const {
  const size_t bpp = this->bytes_per_pixel_();
  const size_t pixels = std::min(frame.length, this->expected_frame_size_) / bpp;
  if (pixels == 0)
    return 0;
  esp_cache_msync(frame.data, frame.length, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  const size_t samples = std::min<size_t>(1024, pixels);
  const size_t step = pixels / samples;
  uint32_t sum = 0;
  for (size_t i = 0; i < samples; i++) {
    const uint8_t *p = frame.data + i * step * bpp;
    switch (this->pixel_format_) {
      case PixelFormat::PIXEL_FORMAT_RGB565: {
        const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
        sum += ((v >> 11) & 0x1F) * 2 + ((v >> 5) & 0x3F) + (v & 0x1F) * 2;
        break;
      }
      case PixelFormat::PIXEL_FORMAT_RGB888:
        sum += p[0] + p[1] + p[2];
        break;
      case PixelFormat::PIXEL_FORMAT_YUV422:
        sum += p[1];
        break;
      default:
        sum += p[0];
        break;
    }
  }
  return static_cast<int32_t>(sum / samples);
}

bool MipiCsiCamera::settled_(uint32_t index) {
  if (this->settle_count_ == UINT16_MAX)
    return true;
  const int32_t level = this->frame_level_(this->buffers_[index]);
  const int32_t last = this->settle_level_;
  this->settle_level_ = level;
  this->settle_count_++;
  const int32_t delta = last < 0 ? INT32_MAX : std::abs(level - last);
  if ((this->settle_count_ >= 3 && delta <= std::max<int32_t>(2, last / 32)) ||
      this->settle_count_ >= 2u * this->framerate_ + 2u) {
    this->settle_count_ = UINT16_MAX;
    return true;
  }
  return false;
}

void MipiCsiCamera::set_preview_buffer(uint8_t *buffer, size_t size) {
  uint8_t *old = this->preview_buffer_.exchange(buffer);
  this->preview_size_ = size;
  this->preview_wanted_.store(false);
  this->preview_ready_.store(false);
  if (old != nullptr && old != buffer) {
    if (this->state_ == State::IDLE) {
      this->preview_released_callback_.call(old);  // no task: nothing copies into it
    } else {
      // An earlier one still waiting is not the task's target any more: back at once; this one once the task is not
      // copying (loop)
      if (this->preview_released_ != nullptr)
        this->preview_released_callback_.call(this->preview_released_);
      this->preview_released_ = old;
    }
  }
  this->enable_loop_soon_any_context();
}

void MipiCsiCamera::set_direct(bool on) {
  this->direct_.store(on);
  if (!on) {
    if (this->direct_given_) {
      this->direct_given_ = false;
      this->direct_callback_.call(nullptr, 0, 0);
    }
    this->release_direct_frame();
  }
  this->enable_loop_soon_any_context();
}

void MipiCsiCamera::release_direct_frame() {
  const int8_t held = this->direct_held_.load();
  if (held < 0)
    return;
  this->direct_given_ = false;
  // Returned first: the task, seeing nothing held, may take the next one; it re-queues this one after its next frame
  this->direct_returned_.store(held);
  this->direct_held_.store(-1);
}

void MipiCsiCamera::request_image(camera::CameraRequester requester) {
  this->single_requesters_ |= (1U << requester);
  this->enable_loop_soon_any_context();
}

void MipiCsiCamera::start_stream(camera::CameraRequester requester) {
  for (auto *listener : this->listeners_) {
    listener->on_stream_start();
  }
  this->stream_requesters_ |= (1U << requester);
  this->enable_loop_soon_any_context();
}

void MipiCsiCamera::stop_stream(camera::CameraRequester requester) {
  for (auto *listener : this->listeners_) {
    listener->on_stream_stop();
  }
  this->stream_requesters_ &= ~(1U << requester);
  this->enable_loop_soon_any_context();
}

bool MipiCsiCamera::h264_spec_(camera::CameraImageSpec *spec) const {
  spec->width = this->width_;
  spec->height = this->height_;
  if (this->pixel_format_ == PixelFormat::PIXEL_FORMAT_RGB565) {
    spec->format = camera::PIXEL_FORMAT_RGB565;
    return true;
  }
  if (this->pixel_format_ == PixelFormat::PIXEL_FORMAT_YUV422) {
    spec->format = camera::PIXEL_FORMAT_UYVY;
    return true;
  }
  return false;
}

void MipiCsiCamera::encode_h264_(const FrameBuffer &frame) {
  this->h264_encoding_.store(true);
  if (this->h264_on_.load()) {
    camera::CameraImageSpec spec{};
    this->h264_spec_(&spec);
    FrameView view(frame.data, this->expected_frame_size_);
    if (this->h264_encoder_->encode_pixels(&spec, &view) == camera::ENCODER_ERROR_SUCCESS) {
      this->h264_length_.store(this->h264_encoder_->get_output_buffer()->get_size());
      this->h264_key_.store(this->h264_encoder_->is_key_frame());
      this->h264_free_.store(false);
      this->h264_ready_.store(true);
    }
  }
  this->h264_encoding_.store(false);
  this->enable_loop_soon_any_context();
  App.wake_loop_threadsafe();
}

void MipiCsiCamera::h264_step_() {
  if (this->h264_encoder_ == nullptr)
    return;
  const uint8_t requesters = this->h264_requesters_.load();
  if (requesters != 0 && !this->h264_started_ && this->state_ == State::RUNNING) {
    camera::CameraImageSpec spec{};
    if (!this->h264_spec_(&spec) || !this->h264_encoder_->start(spec)) {
      ESP_LOGE(TAG, "No H.264 stream for this camera format; the H.264 requests are dropped");
      this->h264_requesters_.store(0);
      return;
    }
    this->h264_started_ = true;
    this->h264_free_.store(true);
    this->h264_ready_.store(false);
    this->h264_on_.store(true);
  }
  if (this->h264_ready_.exchange(false)) {
    auto image = std::make_shared<MipiCsiH264Image>(this, this->h264_encoder_->get_output_buffer()->get_data(),
                                                    this->h264_length_.load(), requesters, this->h264_key_.load());
    for (auto *listener : this->listeners_)
      listener->on_camera_image(image);
  }
  if (requesters == 0 && this->h264_started_) {
    this->h264_on_.store(false);
    if (!this->h264_encoding_.load() && this->h264_free_.load()) {
      this->h264_encoder_->stop();
      this->h264_started_ = false;
    }
  }
}

void MipiCsiCamera::start_stream_as(camera::CameraRequester requester, camera::ImageFormat format) {
  if (format == camera::IMAGE_FORMAT_JPEG) {
    this->start_stream(requester);
    return;
  }
  if (this->h264_encoder_ == nullptr)
    return;
  this->h264_requesters_.fetch_or(static_cast<uint8_t>(1U << requester));
  this->h264_encoder_->request_key_frame();
  this->enable_loop_soon_any_context();
}

void MipiCsiCamera::stop_stream_as(camera::CameraRequester requester, camera::ImageFormat format) {
  if (format == camera::IMAGE_FORMAT_JPEG) {
    this->stop_stream(requester);
    return;
  }
  this->h264_requesters_.fetch_and(static_cast<uint8_t>(~(1U << requester)));
  this->enable_loop_soon_any_context();
}

void MipiCsiCamera::request_key_frame() {
  if (this->h264_encoder_ != nullptr)
    this->h264_encoder_->request_key_frame();
}

MipiCsiH264Image::~MipiCsiH264Image() { this->camera_->on_h264_released(); }

/* ---------------- MipiCsiImageReader ---------------- */

void MipiCsiImageReader::set_image(std::shared_ptr<camera::CameraImage> image) {
  this->image_ = std::move(image);
  this->offset_ = 0;
}

size_t MipiCsiImageReader::available() const {
  if (!this->image_)
    return 0;
  return this->image_->get_data_length() - this->offset_;
}

uint8_t *MipiCsiImageReader::peek_data_buffer() { return this->image_->get_data_buffer() + this->offset_; }

void MipiCsiImageReader::consume_data(size_t consumed) { this->offset_ += consumed; }

void MipiCsiImageReader::return_image() {
  this->image_.reset();
  this->parent_->on_image_returned();
}

}  // namespace esphome::mipi_csi

#endif
