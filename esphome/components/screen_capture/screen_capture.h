#pragma once

#include "esphome/core/defines.h"

#ifdef SVP_CAPTURE

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/components/avi/cast_avi.h"
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#ifdef USE_CAMERA
#include "esphome/components/camera/camera.h"
#endif
#ifdef SVP_CAPTURE_MIC
#include "esphome/components/microphone/microphone_source.h"
#endif

#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_timer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace esphome::screen_capture {

enum class FrameFormat : uint8_t { MJPEG, YUYV, UYVY, RGB565, H264 };

class CaptureHost {
 public:
  static constexpr uint8_t BLOCKED_VIDEO = 1;
  static constexpr uint8_t BLOCKED_STREAM = 2;
  static constexpr uint8_t BLOCKED_DESKTOP = 4;
  virtual uint8_t *capture_alloc(size_t size) = 0;
  virtual void capture_free(uint8_t *piece) = 0;
  virtual uint8_t capture_blocked() = 0;
  virtual void capture_defer(std::function<void()> &&f) = 0;
  virtual std::shared_ptr<ring_buffer::RingBuffer> capture_ring(uint8_t *storage, size_t len) = 0;
  virtual const std::string &capture_relay_url() = 0;
  virtual bool capture_relay_ask(bool start) = 0;
  virtual uint16_t capture_stream_port() = 0;
  virtual bool capture_http(const char *path, const char *payload, char **body, size_t *len) = 0;

 protected:
  ~CaptureHost() = default;
};

class ScreenCapture {
 public:
  explicit ScreenCapture(CaptureHost *host) : host_(host) {}

  enum CastState : uint8_t { CAST_IDLE, CAST_STARTING, CAST_RUNNING, CAST_ENDING };
  enum CastTarget : uint8_t { CAST_LOCAL = 1, CAST_RELAY = 2 };

  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
  void setup();
  void loop();

  void set_screenshot_directory(const std::string &dir) { this->shot_dir_ = dir; }
  const std::string &get_screenshot_directory() const { return this->shot_dir_; }
  void set_screencast_directory(const std::string &dir) { this->cast_dir_ = dir; }
  const std::string &get_screencast_directory() const { return this->cast_dir_; }
  void set_capture_quality(uint8_t quality) {
    this->cap_quality_.store(std::clamp<uint8_t>(quality, 1, 100), std::memory_order_relaxed);
  }
  uint8_t get_capture_quality() const { return this->cap_quality_.load(std::memory_order_relaxed); }
  void set_screencast_fps(uint8_t fps) { this->cast_fps_ = std::clamp<uint8_t>(fps, 1, 30); }
  uint8_t get_screencast_fps() const { return this->cast_fps_; }
  void set_screencast_target(uint8_t target) {
    if ((target & (CAST_LOCAL | CAST_RELAY)) != 0)
      this->cast_target_ = target & (CAST_LOCAL | CAST_RELAY);
  }
  uint8_t get_screencast_target() const { return this->cast_target_; }
  void set_screencast_sound(bool on) { this->cast_sound_ = on; }
  bool get_screencast_sound() const { return this->cast_sound_; }
#ifdef SVP_CAPTURE_MIC
  void set_capture_microphone(microphone::MicrophoneSource *mic) { this->cap_mic_ = mic; }
#endif
  bool screenshot();
  bool screencast_start();
  void screencast_stop();
  void screencast_toggle() {
    if (this->cast_state_ == CAST_IDLE) {
      this->screencast_start();
    } else {
      this->screencast_stop();
    }
  }
  bool camera_screenshot();
  bool camera_record_start(uint16_t width, uint16_t height);
  void set_camera_record_h264(bool on) { this->cast_h264_wanted_ = on; }
  bool get_camera_record_h264() const { return this->cast_h264_wanted_; }
  bool is_camera_recording_h264() const { return this->is_camera_recording() && this->cap_h264_; }
  void camera_record_stop();
  bool is_camera_recording() const {
    return this->cap_camera_ && (this->cast_state_ == CAST_STARTING || this->cast_state_ == CAST_RUNNING);
  }
  void camera_capture_end();
  bool camera_capture_frame(const uint8_t *data, size_t length, FrameFormat format, uint16_t width, uint16_t height,
                            std::function<void()> &&done);
  CastState screencast_state() const { return this->cast_state_; }
  bool screencast_running() const { return this->cast_state_ != CAST_IDLE; }
  jpeg_encoder_handle_t shared_jpeg_encoder();
  bool screencast_mic_on() const {
#ifdef SVP_CAPTURE_MIC
    return this->cast_state_ == CAST_RUNNING && this->cap_mic_on_;
#else
    return false;
#endif
  }
  const std::string &get_screencast_path() const { return this->cast_path_; }
  Trigger<std::string> *get_screenshot_trigger() { return &this->shot_trigger_; }
  Trigger<> *get_screencast_started_trigger() { return &this->cast_started_trigger_; }
  Trigger<std::string> *get_screencast_stopped_trigger() { return &this->cast_stopped_trigger_; }
  void add_on_capture_callback(std::function<void()> &&callback) { this->cap_callbacks_.add(std::move(callback)); }
  bool give_back(uint8_t blocked);
  void relay_exchange(bool start) { this->cast_relay_task_(start); }

 protected:
  class CaptureSink : public lvgl::LvglComponent::FrameCapture {
   public:
    explicit CaptureSink(ScreenCapture *capture) : capture_(capture) {}
    uint8_t *capture_target() override { return this->capture_->cap_target_(); }
    void capture_area(const lv_area_t &area) override { this->capture_->cap_area_(area); }
    void capture_done() override { this->capture_->cap_done_(); }

   protected:
    ScreenCapture *capture_;
  };
  static constexpr size_t CAP_JPEG_BYTES = 512 * 1024;
  static constexpr size_t CAP_PREFIX = 128;
  static constexpr size_t CAP_PCM_BYTES = 32 * 1024;
  static constexpr size_t CAP_INDEX_BYTES = 128 * 1024;
  static constexpr uint16_t CAP_MAX_PICTURES = 250;
  static constexpr size_t CAP_TAIL = avi::CastAvi::CHUNK_HEADER * (CAP_MAX_PICTURES + 2) + CAP_PCM_BYTES;
  static constexpr size_t CAP_JPEG_ROOM = CAP_JPEG_BYTES - CAP_PREFIX - CAP_TAIL;
  static constexpr uint8_t CAP_RECTS = 16;
  static constexpr uint32_t CAP_SYNC_WAIT_MS = 100;
  static constexpr uint32_t CAP_ENCODE_TIMEOUT_MS = 200;
  static constexpr uint32_t CAP_RELAY_WAIT_MS = 10000;
  static constexpr uint32_t CAP_TX_TIMEOUT_MS = 5000;
  static constexpr size_t CAP_TASK_STACK = 4096;
  enum : uint32_t {
    CAPN_TICK = 1 << 0,
    CAPN_SYNC = 1 << 1,
    CAPN_SHOT = 1 << 2,
    CAPN_SINK = 1 << 3,
    CAPN_STOP = 1 << 4,
    CAPN_CAST_SHOT = 1 << 5,
    CAPN_CAM = 1 << 6,
  };
  enum : uint8_t { CAP_OWNER_FREE, CAP_OWNER_ENCODING, CAP_OWNER_SWAPPING };
  enum : uint8_t { CAP_SINK_FILE = 1, CAP_SINK_RELAY = 2 };
  enum : uint8_t { CAP_RELAY_NONE, CAP_RELAY_ASKED, CAP_RELAY_CONNECTING, CAP_RELAY_READY, CAP_RELAY_FAILED };
  enum : uint8_t {
    CF_NONE,
    CF_OPENING,
    CF_HEADER,
    CF_READY,
    CF_BUSY,
    CF_BATCH,
    CF_IX_VIDEO,
    CF_IX_AUDIO,
    CF_SEEK_HEADER,
    CF_HEADER_AGAIN,
    CF_SEEK_PATCH,
    CF_PATCH,
    CF_SEEK_END,
    CF_SEGMENT,
    CF_CLOSING,
    CF_DONE,
    CF_FAILED,
  };

  bool capture_ready_();
  bool capture_take_(bool cast);
  void capture_free_frames_();
  void capture_free_rest_();
  void capture_end_(const char *reason);
  void capture_finished_();
  void capture_notify_(uint32_t bits);
  std::string capture_name_(const std::string &dir, const char *prefix, const char *suffix);

  uint8_t *cap_target_();
  void cap_area_(const lv_area_t &area);
  void cap_done_();

  static void capture_task_entry_(void *param);
  void capture_task_loop_();
  static void capture_tick_cb_(void *arg);
  void capture_sync_();
  bool capture_encode_(const uint8_t *picture, uint8_t *out, uint32_t *len);
  void capture_camera_();
  bool capture_cast_begin_(uint8_t target);
  void capture_cast_encode_();
  void capture_compose_();
  void capture_shot_from_cast_();
  void capture_sink_done_(uint8_t sink);

  void shot_write_();
  void shot_done_(bool ok);
  bool cast_file_open_();
  void cast_file_run_();
  void cast_file_op_(storage::StorageError err);
  void cast_file_result_(storage::StorageError err);
  void cast_file_write_(const uint8_t *data, size_t len, uint8_t next);
  void cast_file_seek_(uint64_t pos, uint8_t next);
  void cast_file_fail_();

  void cast_relay_task_(bool start);
  static void cast_tx_entry_(void *param);
  void cast_tx_loop_();
  bool cast_tx_send_(const uint8_t *data, size_t len);
  void cast_tx_close_();

  CaptureHost *host_;
  lvgl::LvglComponent *lvgl_{nullptr};
  std::string shot_dir_;
  std::string cast_dir_;
  std::atomic<uint8_t> cap_quality_{80};
  uint8_t cast_fps_{25};
  uint8_t cast_target_{CAST_LOCAL};
  bool cast_sound_{true};
#ifdef SVP_CAPTURE_MIC
  microphone::MicrophoneSource *cap_mic_{nullptr};
#endif
  Trigger<std::string> shot_trigger_;
  Trigger<> cast_started_trigger_;
  Trigger<std::string> cast_stopped_trigger_;
  CallbackManager<void()> cap_callbacks_;
  CaptureSink cap_sink_{this};

  CastState cast_state_{CAST_IDLE};
  std::string cast_reason_;
  TaskHandle_t cap_task_{nullptr};
  esp_timer_handle_t cap_timer_{nullptr};
  jpeg_encoder_handle_t cap_encoder_{nullptr};
  ppa_client_handle_t cap_ppa_{nullptr};
  StaticSemaphore_t cap_sync_sem_buf_;
  SemaphoreHandle_t cap_sync_sem_{nullptr};
  uint8_t *cap_frame_[2]{};
  uint8_t *cap_jpeg_[2]{};
  uint8_t *cap_index_{nullptr};
  uint8_t *cap_pcm_{nullptr};
  size_t cap_frame_bytes_{0};
  uint16_t cap_w_{0};
  uint16_t cap_h_{0};
  uint8_t cap_bpp_{2};
  uint8_t cap_back_{1};
  lv_area_t cap_rects_[CAP_RECTS];
  uint8_t cap_rects_n_{0};
  bool cap_need_take_{false};
  bool cap_single_{false};
  lv_area_t cap_sync_rects_[CAP_RECTS];
  uint8_t cap_sync_n_{0};
  std::atomic<bool> cap_sync_busy_{false};
  std::atomic<uint8_t> cap_front_{0};
  std::atomic<uint32_t> cap_swaps_{0};
  std::atomic<uint8_t> cap_owner_{CAP_OWNER_FREE};
  std::atomic<bool> cap_broken_{false};
  uint32_t cap_encoded_swaps_{0};
  uint8_t cap_wjpeg_{0};
  uint32_t cap_wlen_{0};
  bool cap_ejpeg_ready_{false};
  uint32_t cap_elen_{0};
  uint32_t cap_ticks_{0};
  uint32_t cap_enc_errors_{0};
  bool cap_shot_wait_{false};
  std::atomic<uint8_t> cap_jpeg_users_[2]{};
  std::atomic<bool> cap_casting_{false};
  std::atomic<bool> cap_stopping_{false};
  std::atomic<bool> cap_drained_{false};
  std::atomic<bool> cap_shot_cast_{false};
  avi::CastAvi::Batch cap_batch_{};
  const uint8_t *cap_batch_data_{nullptr};
  size_t cap_batch_len_{0};
  std::atomic<uint8_t> cap_batch_sinks_{0};
  std::atomic<bool> cap_file_batch_{false};
  std::atomic<uint8_t> cap_sinks_{0};
  bool cap_relay_asked_any_{false};
  std::atomic<bool> shot_ready_{false};
  const uint8_t *shot_data_{nullptr};
  uint32_t shot_len_{0};
  int8_t shot_jpeg_{-1};
  bool shot_busy_{false};
  uint8_t cam_cap_{0};
  bool cap_camera_{false};
  bool cast_h264_wanted_{false};
  bool cap_h264_{false};
  bool cam_cap_key_{false};
  bool cap_ekey_{false};
  std::atomic<bool> cap_need_key_{true};
#ifdef USE_CAMERA
  class CameraH264Feed : public camera::CameraListener {
   public:
    explicit CameraH264Feed(ScreenCapture *capture) : capture_(capture) {}
    void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override {
      this->capture_->camera_h264_image_(image);
    }

   protected:
    ScreenCapture *capture_;
  };
  CameraH264Feed cam_h264_feed_{this};
  void camera_h264_image_(const std::shared_ptr<camera::CameraImage> &image);
#endif
  bool cam_cap_shot_{false};
  std::atomic<bool> cam_cap_busy_{false};
  std::atomic<bool> cam_cap_finished_{false};
  const uint8_t *cam_cap_data_{nullptr};
  size_t cam_cap_len_{0};
  FrameFormat cam_cap_format_{FrameFormat::RGB565};
  uint16_t cam_cap_w_{0};
  uint16_t cam_cap_h_{0};
  uint32_t cam_cap_last_{0};
  std::function<void()> cam_cap_done_;
  const char *cast_prefix_{"screencast_"};
  uint8_t shot_step_{0};
  bool shot_ok_{false};
  std::string shot_path_;
  storage::StreamHandle shot_handle_{};
  std::string shot_last_stamp_;
  uint8_t shot_same_stamp_{0};
#ifdef SVP_CAPTURE_MIC
  std::shared_ptr<ring_buffer::RingBuffer> cap_ring_;
  std::atomic<ring_buffer::RingBuffer *> cap_ring_raw_{nullptr};
  std::atomic<uint8_t> cap_mic_in_{0};
  bool cap_mic_on_{false};
#endif
  uint32_t cap_rate_{0};
  uint8_t cap_channels_{0};
  avi::CastAvi cap_avi_;
  std::string cast_path_;
  storage::StreamHandle cast_handle_{};
  uint8_t cast_file_{CF_NONE};
  uint8_t cast_file_next_{CF_NONE};
  bool cast_file_final_{false};
  bool cast_file_pending_{false};
  bool cast_op_seek_{false};
  const uint8_t *cast_op_data_{nullptr};
  size_t cast_op_len_{0};
  uint64_t cast_op_pos_{0};
  std::atomic<uint8_t> cap_relay_{CAP_RELAY_NONE};
  std::atomic<uint16_t> cap_relay_port_{0};
  char cap_relay_token_[48]{};
  uint32_t cap_relay_asked_{0};
  TaskHandle_t cap_tx_task_{nullptr};
  int cap_sock_{-1};
  std::atomic<bool> cap_tx_connect_{false};
  std::atomic<bool> cap_tx_send_{false};
  std::atomic<bool> cap_tx_close_{false};
};

}

#endif
