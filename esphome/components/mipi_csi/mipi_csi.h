#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <atomic>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_cam_sensor_types.h>

#include "esphome/components/camera/camera.h"
#include "esphome/components/camera/encoder.h"
#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include "jpeg_encoder.h"

namespace esphome::mipi_csi {

/// Raw capture format requested from the video device.
///
/// Only formats the hardware JPEG encoder can consume are offered, because every captured frame is
/// encoded to JPEG before it leaves this component.
enum class PixelFormat : uint8_t {
  PIXEL_FORMAT_RGB565,
  PIXEL_FORMAT_RGB888,
  PIXEL_FORMAT_YUV422,
  PIXEL_FORMAT_GRAYSCALE,
};

/// Payload handed to the `on_image` automation.
struct CameraImageData {
  uint8_t *data;
  size_t length;
};

/// A single JPEG encoded frame.
///
/// The data is owned by the camera's encoder, so an image stays valid only until the camera reuses
/// the encoder's output buffer. The camera waits for every shared_ptr to be released before it does.
class MipiCsiImage : public camera::CameraImage {
 public:
  MipiCsiImage(uint8_t *data, size_t length, uint8_t requesters)
      : data_(data), length_(length), requesters_(requesters) {}

  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1U << requester)) != 0;
  }

 protected:
  uint8_t *data_;
  size_t length_;
  uint8_t requesters_;
};

class MipiCsiCamera;

class MipiCsiH264Image : public camera::CameraImage {
 public:
  MipiCsiH264Image(MipiCsiCamera *camera, uint8_t *data, size_t length, uint8_t requesters, bool key)
      : camera_(camera), data_(data), length_(length), requesters_(requesters), key_(key) {}
  ~MipiCsiH264Image() override;

  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1U << requester)) != 0;
  }
  camera::ImageFormat get_format() const override { return camera::IMAGE_FORMAT_H264; }
  bool is_key_frame() const override { return this->key_; }

 protected:
  MipiCsiCamera *camera_;
  uint8_t *data_;
  size_t length_;
  uint8_t requesters_;
  bool key_;
};

/// Tracks how far a consumer has read into an image; tells the camera when it gives the image back.
class MipiCsiImageReader : public camera::CameraImageReader {
 public:
  explicit MipiCsiImageReader(MipiCsiCamera *parent) : parent_(parent) {}
  void set_image(std::shared_ptr<camera::CameraImage> image) override;
  size_t available() const override;
  uint8_t *peek_data_buffer() override;
  void consume_data(size_t consumed) override;
  void return_image() override;

 protected:
  MipiCsiCamera *parent_;
  std::shared_ptr<camera::CameraImage> image_;
  size_t offset_{0};
};

/// Memory for the camera's buffers while it streams (frames, JPEG output, the capture task's stack):
/// DMA-capable PSRAM, 128-byte aligned; nullptr when there is no room. Given back with the free function.
using BufferAlloc = uint8_t *(*) (size_t size);
using BufferFree = void (*)(uint8_t *ptr);

/// Camera for MIPI-CSI sensors on the ESP32-P4.
///
/// Frames are captured through the V4L2 interface of Espressif's `esp_video` component and encoded
/// to JPEG by the chip's JPEG peripheral. Sensors with a RAW Bayer output are routed through the
/// ISP, where `esp_ipa` runs auto exposure and auto white balance.
///
/// Everything runs only while an image or a stream is asked for: esp_video, the JPEG engine, the
/// capture task and every buffer are set up then and given back after (the buffers from the allocator).
class MipiCsiCamera final : public camera::Camera {
 public:
  void set_buffer_allocator(BufferAlloc alloc, BufferFree free) {
    this->alloc_ = alloc;
    this->free_ = free;
  }
  /// The HW JPEG encoder engine to encode with (shared with others; nullptr while none exists).
  void set_jpeg_encoder(jpeg_encoder_handle_t (*engine)()) { this->jpeg_engine_ = engine; }
  void set_h264_encoder(camera::Encoder *encoder) { this->h264_encoder_ = encoder; }
  camera::Encoder *get_h264_encoder() const { return this->h264_encoder_; }
  void set_sensor_name(const char *sensor_name) { this->sensor_name_ = sensor_name; }
  void set_resolution(uint16_t width, uint16_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_pixel_format(PixelFormat format) { this->pixel_format_ = format; }
  void set_jpeg_quality(uint8_t quality) { this->jpeg_quality_ = quality; }
  void set_horizontal_flip(bool flip) { this->horizontal_flip_ = flip; }
  void set_vertical_flip(bool flip) { this->vertical_flip_ = flip; }
  void set_framerate(uint8_t framerate) { this->framerate_ = framerate; }
  /// An image control (V4L2_CID_BRIGHTNESS, CONTRAST, SATURATION, SHARPNESS, HFLIP, VFLIP ...): kept for every
  /// start and applied at once while the camera streams. False: no room for another control.
  bool set_control(uint32_t id, int32_t value);
  /// The sensor's flips as controls (applied at once while streaming)
  void set_mirror(bool on);
  void set_upside_down(bool on);
  uint8_t get_framerate() const { return this->framerate_; }
  uint8_t get_jpeg_quality() const { return this->jpeg_quality_; }
  void set_frame_buffer_count(uint8_t count) { this->frame_buffer_count_ = count; }
  void set_i2c_bus(i2c::InternalI2CBus *bus) { this->i2c_bus_ = bus; }
  void set_sccb_frequency(uint32_t frequency) { this->sccb_frequency_ = frequency; }
  void set_init_ldo(bool init_ldo) { this->init_ldo_ = init_ldo; }
  void set_reset_pin(int8_t pin) { this->reset_pin_ = pin; }
  void set_power_pin(int8_t pin) { this->power_pin_ = pin; }
  void set_external_clock(int8_t pin, uint32_t frequency) {
    this->xclk_pin_ = pin;
    this->xclk_frequency_ = frequency;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void add_listener(camera::CameraListener *listener) override { this->listeners_.push_back(listener); }
  camera::CameraImageReader *create_image_reader() override { return new MipiCsiImageReader(this); }  // NOLINT
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;
  bool supports_format(camera::ImageFormat format) const override {
    return format == camera::IMAGE_FORMAT_JPEG || this->h264_encoder_ != nullptr;
  }
  void start_stream_as(camera::CameraRequester requester, camera::ImageFormat format) override;
  void stop_stream_as(camera::CameraRequester requester, camera::ImageFormat format) override;
  void request_key_frame() override;
  void on_h264_released() {
    this->h264_free_.store(true);
    this->enable_loop_soon_any_context();
  }
  /// Main loop: every frame copied raw (the capture format) into @p buffer while it is set; the stream runs while
  /// set. nullptr ends it: the buffer goes back through the released callback once no copy into it is under way.
  void set_preview_buffer(uint8_t *buffer, size_t size);
  size_t frame_size() const { return static_cast<size_t>(this->width_) * this->height_ * this->bytes_per_pixel_(); }
  uint16_t get_width() const { return this->width_; }
  uint16_t get_height() const { return this->height_; }
  void add_on_preview_frame_callback(std::function<void()> &&callback) { this->preview_callback_.add(std::move(callback)); }
  void add_on_preview_released_callback(std::function<void(uint8_t *)> &&callback) {
    this->preview_released_callback_.add(std::move(callback));
  }
  /// Main loop: frames handed over as they are (no copy) while on; the stream runs while on. Each frame comes through
  /// the direct frame callbacks (main loop: data, width, height, in get_pixel_format()); its buffer stays the
  /// reader's until release_direct_frame(), the next one waits for that. data nullptr: the frame given is gone (the
  /// stream ends), whoever reads it drops it at once.
  void set_direct(bool on);
  void release_direct_frame();
  void add_on_direct_frame_callback(std::function<void(const uint8_t *, uint16_t, uint16_t)> &&callback) {
    this->direct_callback_.add(std::move(callback));
  }
  PixelFormat get_pixel_format() const { return this->pixel_format_; }
  /// A reader gave its image back (any task): the main loop checks whether the next frame can be asked for.
  void on_image_returned() { this->enable_loop_soon_any_context(); }

 protected:
  /// One V4L2 capture buffer mapped into our address space.
  struct FrameBuffer {
    uint8_t *data;
    size_t length;
  };

  class FrameView : public camera::Buffer {
   public:
    FrameView(uint8_t *data, size_t length) : data_(data), length_(length) {}
    uint8_t *get_data_buffer() override { return this->data_; }
    size_t get_data_length() override { return this->length_; }

   protected:
    uint8_t *data_;
    size_t length_;
  };

  enum class State : uint8_t { IDLE, RUNNING, STOPPING };

  bool start_external_clock_();
  bool init_video_();
  bool select_sensor_format_();
  bool configure_device_();
  /// Main loop: esp_video, the device, the buffers, the encoder and the capture task for a stream.
  bool start_capture_();
  /// Main loop, once the capture task has parked: everything start_capture_() set up given back.
  void finish_stop_();
  bool queue_buffer_(uint32_t index);
  /// Main loop: the JPEG output (a pool piece) and the engine while API images are asked for; given back after.
  bool open_jpeg_();
  void close_jpeg_();
  /// Logs every MIPI-CSI format the sensor offers, used when the configured resolution is rejected.
  void log_sensor_formats_();
  /// Ranks two sensor formats of the same resolution against each other.
  bool is_better_sensor_format_(const esp_cam_sensor_format_t &candidate, const esp_cam_sensor_format_t &current) const;
  /// Reduces the configured frame rate to one the driver can actually produce.
  uint8_t resolve_framerate_(uint8_t sensor_fps) const;
  /// Applies a V4L2 user control, logging a warning if the sensor does not support it.
  void apply_control_(uint32_t id, int32_t value, const char *name);
  /// Reads back the format the driver settled on, which the JPEG encoder has to match exactly.
  bool read_back_format_(uint32_t expected_fourcc, uint8_t bytes_per_pixel, const char *name);
  bool has_requested_image_() const {
    return this->single_requesters_ != 0 || this->stream_requesters_ != 0 || this->preview_buffer_.load() != nullptr ||
           this->direct_.load() || this->h264_requesters_.load() != 0 || this->h264_started_;
  }
  bool h264_spec_(camera::CameraImageSpec *spec) const;
  void h264_step_();
  void encode_h264_(const FrameBuffer &frame);
  uint8_t bytes_per_pixel_() const;
  bool settled_(uint32_t index);
  int32_t frame_level_(const FrameBuffer &frame) const;

  static void capture_task(void *param);

  const char *sensor_name_{""};
  /// Configured capture size, or zero to keep whatever resolution the sensor driver starts with.
  uint16_t width_{0};
  uint16_t height_{0};
  PixelFormat pixel_format_{PixelFormat::PIXEL_FORMAT_RGB565};
  uint8_t jpeg_quality_{40};
  optional<bool> horizontal_flip_;
  optional<bool> vertical_flip_;
  uint8_t framerate_{10};
  uint8_t frame_buffer_count_{4};
  i2c::InternalI2CBus *i2c_bus_{nullptr};
  uint32_t sccb_frequency_{100000};
  bool init_ldo_{true};
  int8_t reset_pin_{-1};
  int8_t power_pin_{-1};
  int8_t xclk_pin_{-1};
  uint32_t xclk_frequency_{0};

  int fd_{-1};
  // The image controls set at runtime (set_control), applied at each start after the flips
  struct Control {
    uint32_t id;
    int32_t value;
  };
  static constexpr uint8_t MAX_CONTROLS = 8;
  Control controls_[MAX_CONTROLS]{};
  uint8_t control_count_{0};
  /// Frame size the driver reports and the encoder is set up for. A frame that arrives at a
  /// different size did not come out of the pipeline we configured.
  size_t expected_frame_size_{0};
  BufferAlloc alloc_{nullptr};
  BufferFree free_{nullptr};
  jpeg_encoder_handle_t (*jpeg_engine_)(){nullptr};
  State state_{State::IDLE};
  bool video_ready_{false};
  FixedVector<FrameBuffer> buffers_;
  JpegEncoder encoder_;
  uint8_t *jpeg_output_{nullptr};
  // The capture task: its stack from the allocator, its TCB internal (FreeRTOS), both only while streaming
  TaskHandle_t capture_task_handle_{nullptr};
  StaticTask_t *capture_tcb_{nullptr};
  uint8_t *capture_stack_{nullptr};
  /// Set by the main loop to ask the capture task for one frame, cleared by the task when done.
  std::atomic<bool> frame_wanted_{false};
  uint16_t settle_count_{0};
  int32_t settle_level_{-1};
  /// The capture task's result for the main loop: valid once result_ready_ is set (0 = encoding failed).
  std::atomic<size_t> result_length_{0};
  std::atomic<bool> result_ready_{false};
  /// Main loop -> task: stop streaming and park; task -> main loop: parked, everything can be given back.
  std::atomic<bool> stop_wanted_{false};
  std::atomic<bool> parked_{false};
  /// Set by the capture task when the driver failed; the stream is then given up.
  std::atomic<bool> failed_{false};
  // Preview: the caller's buffer (set on the main loop), copied into by the task while preview_busy_
  std::atomic<uint8_t *> preview_buffer_{nullptr};
  size_t preview_size_{0};
  uint8_t *preview_released_{nullptr};  // ended, handed back once the task is not copying
  std::atomic<bool> preview_busy_{false};
  std::atomic<bool> preview_wanted_{false};
  std::atomic<bool> preview_ready_{false};
  CallbackManager<void()> preview_callback_;
  CallbackManager<void(uint8_t *)> preview_released_callback_;
  // Direct frames: the task holds one buffer (its index) until the main loop hands it back (returned, re-queued by
  // the task); given: the reader has it
  std::atomic<bool> direct_{false};
  std::atomic<int8_t> direct_held_{-1};
  std::atomic<int8_t> direct_returned_{-1};
  std::atomic<bool> direct_ready_{false};
  bool direct_given_{false};
  CallbackManager<void(const uint8_t *, uint16_t, uint16_t)> direct_callback_;

  std::shared_ptr<MipiCsiImage> current_image_;
  std::vector<camera::CameraListener *> listeners_;
  std::atomic<uint8_t> single_requesters_{0};
  std::atomic<uint8_t> stream_requesters_{0};
  camera::Encoder *h264_encoder_{nullptr};
  std::atomic<uint8_t> h264_requesters_{0};
  bool h264_started_{false};
  std::atomic<bool> h264_on_{false};
  std::atomic<bool> h264_free_{true};
  std::atomic<bool> h264_encoding_{false};
  std::atomic<bool> h264_ready_{false};
  std::atomic<size_t> h264_length_{0};
  std::atomic<bool> h264_key_{false};
  /// Requesters recorded when the frame currently being captured was asked for.
  uint8_t pending_requesters_{0};
};

class MipiCsiImageTrigger final : public Trigger<CameraImageData>, public camera::CameraListener {
 public:
  explicit MipiCsiImageTrigger(MipiCsiCamera *parent) { parent->add_listener(this); }
  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override {
    this->trigger(CameraImageData{image->get_data_buffer(), image->get_data_length()});
  }
};

class MipiCsiStreamStartTrigger final : public Trigger<>, public camera::CameraListener {
 public:
  explicit MipiCsiStreamStartTrigger(MipiCsiCamera *parent) { parent->add_listener(this); }
  void on_stream_start() override { this->trigger(); }
};

class MipiCsiPreviewFrameTrigger final : public Trigger<> {
 public:
  explicit MipiCsiPreviewFrameTrigger(MipiCsiCamera *parent) {
    parent->add_on_preview_frame_callback([this]() { this->trigger(); });
  }
};

class MipiCsiPreviewReleasedTrigger final : public Trigger<uint8_t *> {
 public:
  explicit MipiCsiPreviewReleasedTrigger(MipiCsiCamera *parent) {
    parent->add_on_preview_released_callback([this](uint8_t *buffer) { this->trigger(buffer); });
  }
};

class MipiCsiStreamStopTrigger final : public Trigger<>, public camera::CameraListener {
 public:
  explicit MipiCsiStreamStopTrigger(MipiCsiCamera *parent) { parent->add_listener(this); }
  void on_stream_stop() override { this->trigger(); }
};

}  // namespace esphome::mipi_csi

#endif
