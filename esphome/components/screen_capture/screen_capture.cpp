// Screenshots and screencasts of what LVGL shows (screen_capture:). LVGL's drawing goes into a
// picture of the screen's own (FrameCapture), the HW JPEG encoder makes the JPEGs on the capture
// task, the files are written through the storage worker on the main loop (each step after the last
// one's completion), the media relay gets the screencast over a connection of its own (cast sender task).
// No encoding of anything else: the sound is the microphone's PCM as it comes.

#include "screen_capture.h"

#ifdef USE_SCREEN_CAPTURE

#include "esphome/core/application.h"
#include "esp_heap_caps.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"

#include "esp_cache.h"

#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace esphome::screen_capture {

static const char *const TAG = "screen_capture";

static const char *const R_VIDEO = "A video started";
static const char *const R_STREAM = "A stream took the panel";
static const char *const R_DESKTOP = "A remote desktop started";
static const char *const R_MEMORY = "Not enough memory for the screencast";
static const char *const R_WRITE = "Writing the screencast's file failed";
static const char *const R_RELAY = "The media relay was not reached";
static const char *const R_RELAY_LOST = "The connection to the media relay was lost";
static const char *const R_FULL = "The screencast reached its longest length";
static const char *const R_CAPTURE = "The screen capture stopped";
static const char *const R_BUSY = "A screenshot is still being written";
static const char *const R_TARGET = "Nowhere to write the screencast";
static const char *const R_CAMERA = "The camera stopped";

static bool create_psram_task(TaskFunction_t entry, const char *name, uint32_t stack, void *arg, UBaseType_t priority,
                              TaskHandle_t *handle, BaseType_t core) {
  return xTaskCreatePinnedToCoreWithCaps(entry, name, stack, arg, priority, handle, core,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

void ScreenCapture::setup() {
  if (this->cast_dir_.empty())
    this->cast_dir_ = this->shot_dir_;
#ifdef USE_CAMERA
  if (camera::Camera::instance() != nullptr)
    camera::Camera::instance()->add_listener(&this->cam_h264_feed_);
#endif
#ifdef USE_SCREEN_CAPTURE_MIC
  if (this->cap_mic_ != nullptr) {
    // Registered once; it writes only while a screencast takes the sound.
    this->cap_mic_->add_data_callback([this](const microphone::AudioCapture &capture) {
      const std::span<const uint8_t> data = capture.data;
      if (this->cap_ring_raw_.load(std::memory_order_acquire) == nullptr)
        return;
      this->cap_mic_in_.fetch_add(1, std::memory_order_acq_rel);
      ring_buffer::RingBuffer *ring = this->cap_ring_raw_.load(std::memory_order_acquire);
      if (ring != nullptr)
        ring->write_without_replacement(data.data(), data.size(), 0, true);
      this->cap_mic_in_.fetch_sub(1, std::memory_order_acq_rel);
    });
  }
#endif
}

jpeg_encoder_handle_t ScreenCapture::shared_jpeg_encoder() {
  if (this->cap_encoder_ == nullptr) {
    jpeg_encode_engine_cfg_t cfg{};
    cfg.timeout_ms = CAP_ENCODE_TIMEOUT_MS;
    if (jpeg_new_encoder_engine(&cfg, &this->cap_encoder_) != ESP_OK) {
      this->cap_encoder_ = nullptr;
      ESP_LOGE(TAG, "No JPEG encoder");
    }
  }
  return this->cap_encoder_;
}

bool ScreenCapture::capture_ready_() {
  if (this->lvgl_ == nullptr)
    return false;
  if (this->cap_sync_sem_ == nullptr)
    this->cap_sync_sem_ = xSemaphoreCreateBinaryStatic(&this->cap_sync_sem_buf_);
  if (this->shared_jpeg_encoder() == nullptr)
    return false;
  if (this->cap_ppa_ == nullptr) {
    ppa_client_config_t cfg{};
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;
    if (ppa_register_client(&cfg, &this->cap_ppa_) != ESP_OK)
      this->cap_ppa_ = nullptr;  // copied by the CPU then
  }
  if (this->cap_task_ == nullptr &&
      !create_psram_task(&ScreenCapture::capture_task_entry_, "screen_capture", CAP_TASK_STACK, this, 3,
                         &this->cap_task_, 0)) {
    this->cap_task_ = nullptr;
    ESP_LOGE(TAG, "No capture task");
    return false;
  }
  if (this->cap_timer_ == nullptr) {
    esp_timer_create_args_t args{};
    args.callback = &ScreenCapture::capture_tick_cb_;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "screen_cast";
    if (esp_timer_create(&args, &this->cap_timer_) != ESP_OK) {
      this->cap_timer_ = nullptr;
      return false;
    }
  }
  this->cap_w_ = this->lvgl_->get_width();
  this->cap_h_ = this->lvgl_->get_height();
  this->cap_bpp_ = static_cast<uint8_t>(lvgl::LvglComponent::capture_bpp());
  this->cap_frame_bytes_ = this->lvgl_->capture_bytes();
  return true;
}

bool ScreenCapture::capture_take_(bool cast) {
  auto take = [this](size_t size) { return this->host_->capture_alloc(size); };
  bool ok = (this->cap_frame_[0] = take(this->cap_frame_bytes_)) != nullptr &&
            (this->cap_jpeg_[0] = take(CAP_JPEG_BYTES)) != nullptr;
  if (ok && cast) {
    ok = (this->cap_frame_[1] = take(this->cap_frame_bytes_)) != nullptr &&
         (this->cap_jpeg_[1] = take(CAP_JPEG_BYTES)) != nullptr &&
         (this->cap_index_ = take(CAP_INDEX_BYTES)) != nullptr;
#ifdef USE_SCREEN_CAPTURE_MIC
    if (ok && this->cast_sound_ && this->cap_mic_ != nullptr)
      ok = (this->cap_pcm_ = take(CAP_PCM_BYTES)) != nullptr;
#endif
  }
  if (!ok) {
    ESP_LOGW(TAG, "PSRAM: no room for the %s (largest free %zu)", cast ? "screencast" : "screenshot",
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    this->capture_free_frames_();
    this->capture_free_rest_();
  }
  return ok;
}

void ScreenCapture::capture_free_frames_() {
  for (auto *&piece : this->cap_frame_) {
    if (piece != nullptr)
      this->host_->capture_free(piece);
    piece = nullptr;
  }
  for (auto *&piece : this->cap_jpeg_) {
    if (piece != nullptr)
      this->host_->capture_free(piece);
    piece = nullptr;
  }
}

void ScreenCapture::capture_free_rest_() {
#ifdef USE_SCREEN_CAPTURE_MIC
  this->cap_ring_.reset();
#endif
  if (this->cap_pcm_ != nullptr)
    this->host_->capture_free(this->cap_pcm_);
  this->cap_pcm_ = nullptr;
  if (this->cap_index_ != nullptr)
    this->host_->capture_free(this->cap_index_);
  this->cap_index_ = nullptr;
}

void ScreenCapture::capture_notify_(uint32_t bits) {
  if (this->cap_task_ != nullptr)
    xTaskNotify(this->cap_task_, bits, eSetBits);
}

void ScreenCapture::capture_tick_cb_(void *arg) {
  static_cast<ScreenCapture *>(arg)->capture_notify_(CAPN_TICK);
}

// <dir>/<prefix>2026-10-18_14-05-09<suffix>, a number after a second one in the same second; the
// uptime while the clock is not set.
std::string ScreenCapture::capture_name_(const std::string &dir, const char *prefix, const char *suffix) {
  char stamp[40];
  const time_t now = ::time(nullptr);
  struct tm local {};
  if (now > 1600000000 && localtime_r(&now, &local) != nullptr) {
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
  } else {
    std::snprintf(stamp, sizeof(stamp), "uptime-%" PRIu32, millis());
  }
  std::string name = dir + "/" + prefix + stamp;
  if (this->shot_last_stamp_ == stamp) {
    char more[8];
    std::snprintf(more, sizeof(more), "-%u", static_cast<unsigned>(++this->shot_same_stamp_ + 1));
    name += more;
  } else {
    this->shot_last_stamp_ = stamp;
    this->shot_same_stamp_ = 0;
  }
  return name + suffix;
}

//==================================================================================================
// LVGL thread: the pictures
//==================================================================================================

uint8_t *ScreenCapture::cap_target_() {
  if (this->cap_single_)
    return this->cap_frame_[0];
  if (this->cap_broken_.load(std::memory_order_relaxed))
    return nullptr;
  if (this->cap_need_take_) {
    // The changed areas are being copied into this one: LVGL draws into it once that is done.
    if (xSemaphoreTake(this->cap_sync_sem_, pdMS_TO_TICKS(CAP_SYNC_WAIT_MS)) != pdTRUE) {
      this->cap_broken_.store(true, std::memory_order_release);
      return nullptr;
    }
    this->cap_need_take_ = false;
  }
  return this->cap_frame_[this->cap_back_];
}

void ScreenCapture::cap_area_(const lv_area_t &area) {
  if (this->cap_single_)
    return;
  if (this->cap_rects_n_ < CAP_RECTS) {
    this->cap_rects_[this->cap_rects_n_++] = area;
    return;
  }
  // Full: one rectangle over all of them.
  lv_area_t &all = this->cap_rects_[0];
  for (uint8_t i = 1; i < CAP_RECTS; i++) {
    all.x1 = std::min(all.x1, this->cap_rects_[i].x1);
    all.y1 = std::min(all.y1, this->cap_rects_[i].y1);
    all.x2 = std::max(all.x2, this->cap_rects_[i].x2);
    all.y2 = std::max(all.y2, this->cap_rects_[i].y2);
  }
  all.x1 = std::min(all.x1, area.x1);
  all.y1 = std::min(all.y1, area.y1);
  all.x2 = std::max(all.x2, area.x2);
  all.y2 = std::max(all.y2, area.y2);
  this->cap_rects_n_ = 1;
}

void ScreenCapture::cap_done_() {
  if (this->cap_single_) {
    // The screenshot's whole picture is drawn.
    this->cap_single_ = false;
    this->lvgl_->set_frame_capture(nullptr);
    this->capture_notify_(CAPN_SHOT);
    return;
  }
  if (this->cap_rects_n_ == 0)
    return;
  uint8_t idle = CAP_OWNER_FREE;
  if (!this->cap_owner_.compare_exchange_strong(idle, CAP_OWNER_SWAPPING, std::memory_order_acq_rel))
    return;  // the encoder reads the front one: LVGL goes on drawing into this one
  const uint8_t drawn = this->cap_back_;
  std::memcpy(this->cap_sync_rects_, this->cap_rects_, this->cap_rects_n_ * sizeof(lv_area_t));
  this->cap_sync_n_ = this->cap_rects_n_;
  this->cap_rects_n_ = 0;
  this->cap_back_ = drawn ^ 1;
  this->cap_front_.store(drawn, std::memory_order_release);
  this->cap_swaps_.fetch_add(1, std::memory_order_acq_rel);
  this->cap_need_take_ = true;
  this->cap_sync_busy_.store(true, std::memory_order_release);
  this->cap_owner_.store(CAP_OWNER_FREE, std::memory_order_release);
  this->capture_notify_(CAPN_SYNC);
}

//==================================================================================================
// Capture task
//==================================================================================================

void ScreenCapture::capture_task_entry_(void *param) {
  static_cast<ScreenCapture *>(param)->capture_task_loop_();
}

void ScreenCapture::capture_task_loop_() {
  while (true) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
    if (bits & CAPN_SYNC)
      this->capture_sync_();
    if (bits & CAPN_CAM)
      this->capture_camera_();
    if (bits & CAPN_SHOT) {
      // A screenshot of its own (no screencast): its picture into its JPEG buffer.
      uint32_t len = 0;
      const bool ok = this->capture_encode_(this->cap_frame_[0], this->cap_jpeg_[0] + CAP_PREFIX, &len);
      this->shot_data_ = this->cap_jpeg_[0] + CAP_PREFIX;
      this->shot_len_ = ok ? len : 0;
      this->shot_ready_.store(true, std::memory_order_release);
      App.wake_loop_threadsafe();
    }
    if (!this->cap_casting_.load(std::memory_order_acquire))
      continue;
    if (bits & CAPN_CAST_SHOT)
      this->capture_shot_from_cast_();
    const bool stopping = this->cap_stopping_.load(std::memory_order_acquire);
    if ((bits & CAPN_TICK) && !stopping) {
      this->cap_ticks_++;
      this->capture_cast_encode_();
    }
    if (!stopping) {
      this->capture_compose_();
    } else if (this->cap_batch_sinks_.load(std::memory_order_acquire) == 0 &&
               !this->cap_drained_.load(std::memory_order_relaxed)) {
      this->cap_drained_.store(true, std::memory_order_release);
      App.wake_loop_threadsafe();
    }
  }
}

// The areas LVGL drew into the front picture since the last swap, into the new back one.
void ScreenCapture::capture_sync_() {
  if (!this->cap_sync_busy_.load(std::memory_order_acquire))
    return;
  const uint8_t front = this->cap_front_.load(std::memory_order_acquire);
  const uint8_t *src = this->cap_frame_[front];
  uint8_t *dst = this->cap_frame_[front ^ 1];
  const size_t stride = static_cast<size_t>(this->cap_w_) * this->cap_bpp_;
  for (uint8_t i = 0; i < this->cap_sync_n_; i++) {
    const lv_area_t &r = this->cap_sync_rects_[i];
    const uint16_t w = static_cast<uint16_t>(r.x2 - r.x1 + 1);
    const uint16_t h = static_cast<uint16_t>(r.y2 - r.y1 + 1);
    if (this->cap_ppa_ != nullptr) {
      ppa_srm_oper_config_t cfg{};
      cfg.in.buffer = src;
      cfg.in.pic_w = this->cap_w_;
      cfg.in.pic_h = this->cap_h_;
      cfg.in.block_w = w;
      cfg.in.block_h = h;
      cfg.in.block_offset_x = r.x1;
      cfg.in.block_offset_y = r.y1;
      cfg.out.buffer = dst;
      cfg.out.buffer_size = this->cap_frame_bytes_;
      cfg.out.pic_w = this->cap_w_;
      cfg.out.pic_h = this->cap_h_;
      cfg.out.block_offset_x = r.x1;
      cfg.out.block_offset_y = r.y1;
      cfg.in.srm_cm = this->cap_bpp_ == 2 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888;
      cfg.out.srm_cm = cfg.in.srm_cm;
      cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
      cfg.scale_x = 1.0f;
      cfg.scale_y = 1.0f;
      cfg.mode = PPA_TRANS_MODE_BLOCKING;
      if (ppa_do_scale_rotate_mirror(this->cap_ppa_, &cfg) == ESP_OK)
        continue;
    }
    for (int32_t y = r.y1; y <= r.y2; y++) {
      const size_t at = static_cast<size_t>(y) * stride + static_cast<size_t>(r.x1) * this->cap_bpp_;
      std::memcpy(dst + at, src + at, static_cast<size_t>(w) * this->cap_bpp_);
    }
    esp_cache_msync(dst + static_cast<size_t>(r.y1) * stride, static_cast<size_t>(h) * stride,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  }
  this->cap_sync_busy_.store(false, std::memory_order_release);
  xSemaphoreGive(this->cap_sync_sem_);
}

bool ScreenCapture::capture_encode_(const uint8_t *picture, uint8_t *out, uint32_t *len) {
  jpeg_encode_cfg_t cfg{};
  cfg.width = this->cap_w_;
  cfg.height = this->cap_h_;
  // LVGL's RGB565 (little-endian) and RGB888 (B, G, R) are the encoder's own input orders.
  cfg.src_type = this->cap_bpp_ == 2 ? JPEG_ENCODE_IN_FORMAT_RGB565 : JPEG_ENCODE_IN_FORMAT_RGB888;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
  cfg.image_quality = this->cap_quality_.load(std::memory_order_relaxed);
  const uint32_t in_size = static_cast<uint32_t>(this->cap_w_) * this->cap_h_ * this->cap_bpp_;
  if (jpeg_encoder_process(this->cap_encoder_, &cfg, picture, in_size, out, CAP_JPEG_ROOM, len) != ESP_OK) {
    this->cap_enc_errors_++;
    return false;
  }
  return true;
}

void ScreenCapture::capture_camera_() {
  if (!this->cam_cap_busy_.load(std::memory_order_acquire))
    return;
  uint8_t *out = nullptr;
  if (this->cam_cap_shot_) {
    out = this->cap_jpeg_[0] + CAP_PREFIX;
  } else if (this->cap_casting_.load(std::memory_order_acquire)) {
    const uint8_t target = this->cap_wjpeg_ ^ 1;
    if (this->cap_jpeg_users_[target].load(std::memory_order_acquire) == 0)
      out = this->cap_jpeg_[target] + CAP_PREFIX;
  }
  uint32_t len = 0;
  bool ok = false;
  if (this->cam_cap_format_ == FrameFormat::H264) {
    if (this->cap_ejpeg_ready_)
      return;
    if (out != nullptr && this->cam_cap_len_ <= CAP_JPEG_ROOM &&
        (this->cam_cap_key_ || !this->cap_need_key_.load(std::memory_order_acquire))) {
      std::memcpy(out, this->cam_cap_data_, this->cam_cap_len_);
      len = static_cast<uint32_t>(this->cam_cap_len_);
      ok = true;
      this->cap_need_key_.store(false, std::memory_order_release);
      this->cap_ekey_ = this->cam_cap_key_;
    } else {
      this->cap_need_key_.store(true, std::memory_order_release);
#ifdef USE_CAMERA
      camera::Camera::instance()->request_key_frame();
#endif
    }
  } else if (out != nullptr) {
    if (this->cam_cap_format_ == FrameFormat::MJPEG) {
      ok = this->cam_cap_len_ <= CAP_JPEG_ROOM;
      if (ok) {
        std::memcpy(out, this->cam_cap_data_, this->cam_cap_len_);
        len = static_cast<uint32_t>(this->cam_cap_len_);
      }
    } else {
      jpeg_encode_cfg_t cfg{};
      cfg.width = this->cam_cap_w_;
      cfg.height = this->cam_cap_h_;
      cfg.src_type = this->cam_cap_format_ == FrameFormat::RGB565 ? JPEG_ENCODE_IN_FORMAT_RGB565
                                                                         : JPEG_ENCODE_IN_FORMAT_YUV422;
      cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
      cfg.image_quality = this->cap_quality_.load(std::memory_order_relaxed);
      const uint32_t in_size = static_cast<uint32_t>(this->cam_cap_w_) * this->cam_cap_h_ * 2;
      ok = jpeg_encoder_process(this->cap_encoder_, &cfg, this->cam_cap_data_, in_size, out, CAP_JPEG_ROOM, &len) ==
           ESP_OK;
      if (!ok)
        this->cap_enc_errors_++;
    }
  }
  if (this->cam_cap_shot_) {
    this->shot_data_ = out;
    this->shot_len_ = ok ? len : 0;
    this->shot_ready_.store(true, std::memory_order_release);
  } else if (ok) {
    this->cap_elen_ = len;
    this->cap_ejpeg_ready_ = true;
  }
  this->cam_cap_finished_.store(true, std::memory_order_release);
  App.wake_loop_threadsafe();
}

// A tick: the front picture into the JPEG buffer not written out, when it changed since.
void ScreenCapture::capture_cast_encode_() {
  const uint32_t swaps = this->cap_swaps_.load(std::memory_order_acquire);
  if (swaps == 0 || swaps == this->cap_encoded_swaps_)
    return;
  const uint8_t target = this->cap_wjpeg_ ^ 1;
  if (this->cap_jpeg_users_[target].load(std::memory_order_acquire) != 0)
    return;  // a screenshot still writes it
  uint8_t idle = CAP_OWNER_FREE;
  if (!this->cap_owner_.compare_exchange_strong(idle, CAP_OWNER_ENCODING, std::memory_order_acq_rel))
    return;  // swapping right now: the next tick
  const uint8_t front = this->cap_front_.load(std::memory_order_acquire);
  const uint32_t encoded = this->cap_swaps_.load(std::memory_order_acquire);
  uint32_t len = 0;
  const bool ok = this->capture_encode_(this->cap_frame_[front], this->cap_jpeg_[target] + CAP_PREFIX, &len);
  this->cap_owner_.store(CAP_OWNER_FREE, std::memory_order_release);
  if (!ok)
    return;
  this->cap_elen_ = len;
  this->cap_ejpeg_ready_ = true;
  this->cap_encoded_swaps_ = encoded;
}

// One batch at a time: the ticks since the last one, the newest JPEG (else an empty chunk for the
// same picture) and the sound since.
void ScreenCapture::capture_compose_() {
  if ((this->cap_ticks_ == 0 && !(this->cap_h264_ && this->cap_ejpeg_ready_)) ||
      this->cap_batch_sinks_.load(std::memory_order_acquire) != 0 ||
      this->cap_sinks_.load(std::memory_order_acquire) == 0)
    return;
  const bool fresh = this->cap_ejpeg_ready_;
  if (fresh) {
    this->cap_wjpeg_ ^= 1;
    this->cap_wlen_ = this->cap_elen_;
    this->cap_ejpeg_ready_ = false;
  }
  if (this->cap_wlen_ == 0)
    return;  // no picture yet
  uint8_t *buf = this->cap_jpeg_[this->cap_wjpeg_];
  avi::CastAvi::Batch batch{};
  batch.pictures = static_cast<uint16_t>(
      std::max<uint32_t>(1, std::min<uint32_t>(this->cap_ticks_, CAP_MAX_PICTURES)));
  this->cap_ticks_ -= std::min<uint32_t>(this->cap_ticks_, batch.pictures);
  uint8_t *pic = buf + CAP_PREFIX;
  if (fresh) {
    batch.jpeg_len = this->cap_wlen_;
    batch.key = !this->cap_h264_ || this->cap_ekey_;
  } else {
    batch.key = !this->cap_h264_;
    // After the JPEG kept there: a screenshot may still write that one.
    pic += this->cap_wlen_ + (this->cap_wlen_ & 1) + avi::CastAvi::CHUNK_HEADER;
  }
#ifdef USE_SCREEN_CAPTURE_MIC
  ring_buffer::RingBuffer *ring = this->cap_ring_raw_.load(std::memory_order_acquire);
  if (ring != nullptr && this->cap_channels_ != 0) {
    const size_t block = static_cast<size_t>(this->cap_channels_) * 2;
    const size_t available = std::min(ring->available(), CAP_PCM_BYTES);
    batch.audio_len = static_cast<uint32_t>(available / block * block);
  }
#endif
  uint8_t *sound = avi::CastAvi::lay_out(pic, batch);
#ifdef USE_SCREEN_CAPTURE_MIC
  if (sound != nullptr) {
    const size_t got = ring->read(sound, batch.audio_len, 0);
    if (got < batch.audio_len)
      std::memset(sound + got, 0, batch.audio_len - got);
  }
#else
  (void) sound;
#endif
  this->cap_batch_ = batch;
  this->cap_batch_data_ = pic - avi::CastAvi::CHUNK_HEADER;
  this->cap_batch_len_ = batch.bytes();
  if (fresh && this->cap_shot_wait_) {
    this->cap_shot_wait_ = false;
    this->capture_shot_from_cast_();
  }
  const uint8_t sinks = this->cap_sinks_.load(std::memory_order_acquire);
  this->cap_batch_sinks_.store(sinks, std::memory_order_release);
  if (sinks & CAP_SINK_FILE) {
    this->cap_file_batch_.store(true, std::memory_order_release);
    App.wake_loop_threadsafe();
  }
  if (sinks & CAP_SINK_RELAY) {
    this->cap_tx_send_.store(true, std::memory_order_release);
    xTaskNotifyGive(this->cap_tx_task_);
  }
  if (fresh && this->cap_h264_ && this->cam_cap_busy_.load(std::memory_order_acquire))
    this->capture_camera_();
}

// Casting: a screenshot is the newest JPEG written out.
void ScreenCapture::capture_shot_from_cast_() {
  if (!this->cap_shot_cast_.load(std::memory_order_acquire))
    return;
  if (this->cap_wlen_ == 0) {
    this->cap_shot_wait_ = true;
    return;
  }
  this->cap_shot_cast_.store(false, std::memory_order_relaxed);
  this->cap_jpeg_users_[this->cap_wjpeg_].fetch_add(1, std::memory_order_acq_rel);
  this->shot_jpeg_ = static_cast<int8_t>(this->cap_wjpeg_);
  this->shot_data_ = this->cap_jpeg_[this->cap_wjpeg_] + CAP_PREFIX;
  this->shot_len_ = this->cap_wlen_;
  this->shot_ready_.store(true, std::memory_order_release);
  App.wake_loop_threadsafe();
}

void ScreenCapture::capture_sink_done_(uint8_t sink) {
  if ((this->cap_batch_sinks_.fetch_and(static_cast<uint8_t>(~sink), std::memory_order_acq_rel) & ~sink) == 0)
    this->capture_notify_(CAPN_SINK);
}

//==================================================================================================
// Main loop
//==================================================================================================

bool ScreenCapture::screenshot() {
  this->host_->capture_wake();
  auto refuse = [this](const char *why) {
    ESP_LOGW(TAG, "No screenshot: %s", why);
    this->shot_trigger_.trigger(std::string());
    return false;
  };
  if (this->shot_busy_)
    return refuse(R_BUSY);
  if (this->shot_dir_.empty() || this->lvgl_ == nullptr || this->lvgl_->is_paused()) {
    this->shot_trigger_.trigger(std::string());  // nothing of LVGL's to take
    return false;
  }
  this->shot_path_ = this->capture_name_(this->shot_dir_, "screenshot_", ".jpg");
  if (this->cast_state_ == CAST_RUNNING) {
    // The screencast's newest JPEG.
    this->shot_busy_ = true;
    this->cap_shot_cast_.store(true, std::memory_order_release);
    this->capture_notify_(CAPN_CAST_SHOT);
    return true;
  }
  if (this->cast_state_ != CAST_IDLE)
    return refuse("the screencast starts or ends");
  if (!this->capture_ready_() || !this->capture_take_(false))
    return refuse("no memory for it");
  this->shot_busy_ = true;
  this->shot_jpeg_ = -1;
  if (this->lvgl_->copy_shown(this->cap_frame_[0])) {
    this->capture_notify_(CAPN_SHOT);
  } else {
    // LVGL draws the screen anew into it.
    this->cap_single_ = true;
    this->lvgl_->set_frame_capture(&this->cap_sink_);
    lv_obj_invalidate(lv_screen_active());
  }
  return true;
}

void ScreenCapture::shot_write_() {
  storage::StorageWorker *worker = storage::global_storage_worker;
  const char *dir_rel = nullptr;
  storage::PathStorage *dir = storage::global_storage_registry != nullptr && worker != nullptr
                                  ? storage::global_storage_registry->resolve_path(this->shot_dir_.c_str(), &dir_rel)
                                  : nullptr;
  if (dir == nullptr || this->shot_len_ == 0) {
    this->shot_done_(false);
    return;
  }
  if (dir_rel != nullptr && dir_rel[0] != '\0' && !worker->has_active_task_io(dir))
    dir->mkdir(dir_rel);  // already there: an error, and nothing to do
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->shot_path_.c_str(), &rel);
  if (ps == nullptr) {
    this->shot_done_(false);
    return;
  }
  this->shot_step_ = 1;
  this->shot_ok_ = true;
  const storage::StorageError err = worker->begin_write(ps, rel, &this->shot_handle_, [this](storage::StorageError e) {
    this->host_->capture_defer([this, e]() {
      if (e != storage::StorageError::STORAGE_ERROR_OK) {
        this->shot_done_(false);
        return;
      }
      auto close = [this]() {
        const storage::StorageError closing =
            storage::global_storage_worker->end_write(this->shot_handle_, [this](storage::StorageError c) {
              const bool ok = this->shot_ok_ && c == storage::StorageError::STORAGE_ERROR_OK;
              this->host_->capture_defer([this, ok]() { this->shot_done_(ok); });
            });
        if (closing != storage::StorageError::STORAGE_ERROR_OK)
          this->shot_done_(false);
      };
      const storage::StorageError written = storage::global_storage_worker->write_chunk(
          this->shot_handle_, this->shot_data_, this->shot_len_, [this, close](storage::StorageError w) {
            this->shot_ok_ = w == storage::StorageError::STORAGE_ERROR_OK;
            this->host_->capture_defer(close);
          });
      if (written != storage::StorageError::STORAGE_ERROR_OK) {
        this->shot_ok_ = false;
        close();
      }
    });
  });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->shot_done_(false);
}

void ScreenCapture::shot_done_(bool ok) {
  if (this->shot_jpeg_ >= 0) {
    this->cap_jpeg_users_[this->shot_jpeg_].fetch_sub(1, std::memory_order_acq_rel);
    this->shot_jpeg_ = -1;
    this->capture_notify_(CAPN_SINK);
  } else if (this->cast_state_ == CAST_IDLE) {
    this->capture_free_frames_();
  }
  this->shot_busy_ = false;
  this->shot_step_ = 0;
  if (ok) {
    ESP_LOGI(TAG, "Screenshot %s (%" PRIu32 " bytes)", this->shot_path_.c_str(), this->shot_len_);
    this->shot_trigger_.trigger(this->shot_path_);
  } else {
    ESP_LOGW(TAG, "Screenshot %s not written", this->shot_path_.c_str());
    this->shot_trigger_.trigger(std::string());
  }
}

bool ScreenCapture::screencast_start() {
  this->host_->capture_wake();
  auto refuse = [this](const char *why) {
    ESP_LOGW(TAG, "No screencast: %s", why);
    this->cast_stopped_trigger_.trigger(std::string(why));
    return false;
  };
  if (this->cast_state_ != CAST_IDLE)
    return false;
  if (this->shot_busy_)
    return refuse(R_BUSY);
  if (this->lvgl_ == nullptr || this->lvgl_->is_paused())
    return false;  // nothing of LVGL's to take
  const uint8_t blocked = this->host_->capture_blocked();
  if (blocked & CaptureHost::BLOCKED_VIDEO)
    return refuse(R_VIDEO);
  if (blocked & CaptureHost::BLOCKED_STREAM)
    return refuse(R_STREAM);
  if (blocked & CaptureHost::BLOCKED_DESKTOP)
    return refuse(R_DESKTOP);
  uint8_t target = this->cast_target_;
  if (*this->host_->capture_relay_url() == '\0')
    target &= ~CAST_RELAY;
  if (this->cast_dir_.empty())
    target &= ~CAST_LOCAL;
  if (target == 0)
    return refuse(R_TARGET);
  if (!this->capture_ready_())
    return refuse(R_CAPTURE);
  if (!this->capture_take_(true))
    return refuse(R_MEMORY);

  // Both pictures as the screen shows it now, else LVGL draws it anew.
  this->cap_rects_n_ = 0;
  this->cap_need_take_ = false;
  this->cap_single_ = false;
  this->cap_broken_.store(false, std::memory_order_relaxed);
  this->cap_sync_busy_.store(false, std::memory_order_relaxed);
  this->cap_owner_.store(CAP_OWNER_FREE, std::memory_order_relaxed);
  xSemaphoreTake(this->cap_sync_sem_, 0);
  if (this->lvgl_->copy_shown(this->cap_frame_[0]) && this->lvgl_->copy_shown(this->cap_frame_[1])) {
    this->cap_front_.store(0, std::memory_order_relaxed);
    this->cap_back_ = 1;
    this->cap_swaps_.store(1, std::memory_order_relaxed);
  } else {
    this->cap_front_.store(1, std::memory_order_relaxed);
    this->cap_back_ = 0;
    this->cap_swaps_.store(0, std::memory_order_relaxed);
    lv_obj_invalidate(lv_screen_active());
  }
  this->cap_camera_ = false;
  this->cap_h264_ = false;
  this->cast_prefix_ = "screencast_";
  return this->capture_cast_begin_(target);
}

bool ScreenCapture::camera_screenshot() {
  this->host_->capture_wake();
  auto refuse = [this](const char *why) {
    ESP_LOGW(TAG, "No camera screenshot: %s", why);
    this->shot_trigger_.trigger(std::string());
    return false;
  };
  if (this->shot_busy_ || this->cam_cap_ != 0)
    return refuse(R_BUSY);
  if (this->shot_dir_.empty())
    return refuse("no screenshot folder");
  this->shot_path_ = this->capture_name_(this->shot_dir_, "camera_", ".jpg");
  if (this->cap_camera_ && this->cast_state_ == CAST_RUNNING) {
    this->shot_busy_ = true;
    this->cap_shot_cast_.store(true, std::memory_order_release);
    this->capture_notify_(CAPN_CAST_SHOT);
    return true;
  }
  if (this->cast_state_ != CAST_IDLE)
    return refuse("a screencast runs");
  if (!this->capture_ready_())
    return refuse(R_CAPTURE);
  this->cap_jpeg_[0] = this->host_->capture_alloc(CAP_JPEG_BYTES);
  if (this->cap_jpeg_[0] == nullptr)
    return refuse("no memory for it");
  this->shot_busy_ = true;
  this->shot_jpeg_ = -1;
  this->cam_cap_ = 1;
  return true;
}

bool ScreenCapture::camera_record_start(uint16_t width, uint16_t height) {
  this->host_->capture_wake();
  auto refuse = [this](const char *why) {
    ESP_LOGW(TAG, "No camera recording: %s", why);
    this->cast_stopped_trigger_.trigger(std::string(why));
    return false;
  };
  if (this->cast_state_ != CAST_IDLE)
    return false;
  if (this->shot_busy_)
    return refuse(R_BUSY);
  if (width == 0 || height == 0)
    return refuse(R_CAMERA);
  if (this->cast_dir_.empty())
    return refuse(R_TARGET);
  if (!this->capture_ready_())
    return refuse(R_CAPTURE);
  auto take = [this](size_t size) { return this->host_->capture_alloc(size); };
  bool ok = (this->cap_jpeg_[0] = take(CAP_JPEG_BYTES)) != nullptr &&
            (this->cap_jpeg_[1] = take(CAP_JPEG_BYTES)) != nullptr &&
            (this->cap_index_ = take(CAP_INDEX_BYTES)) != nullptr;
#ifdef USE_SCREEN_CAPTURE_MIC
  if (ok && this->cast_sound_ && this->cap_mic_ != nullptr)
    ok = (this->cap_pcm_ = take(CAP_PCM_BYTES)) != nullptr;
#endif
  if (!ok) {
    this->capture_free_frames_();
    this->capture_free_rest_();
    return refuse(R_MEMORY);
  }
  this->cap_w_ = width;
  this->cap_h_ = height;
  this->cap_rects_n_ = 0;
  this->cap_need_take_ = false;
  this->cap_single_ = false;
  this->cap_broken_.store(false, std::memory_order_relaxed);
  this->cap_sync_busy_.store(false, std::memory_order_relaxed);
  this->cap_owner_.store(CAP_OWNER_FREE, std::memory_order_relaxed);
  this->cap_front_.store(0, std::memory_order_relaxed);
  this->cap_back_ = 1;
  this->cap_swaps_.store(0, std::memory_order_relaxed);
  this->cam_cap_last_ = 0;
  this->cap_camera_ = true;
  this->cap_h264_ = false;
#ifdef USE_CAMERA
  camera::Camera *cam = camera::Camera::instance();
  if (this->cast_h264_wanted_) {
    if (cam != nullptr && cam->supports_format(camera::IMAGE_FORMAT_H264)) {
      this->cap_h264_ = true;
    } else {
      ESP_LOGW(TAG, "The camera has no H.264 stream: the recording is MJPEG");
    }
  }
#endif
  this->cap_need_key_.store(true, std::memory_order_release);
  this->cast_prefix_ = "camera_";
  if (!this->capture_cast_begin_(CAST_LOCAL)) {
    this->cap_h264_ = false;
    return false;
  }
#ifdef USE_CAMERA
  if (this->cap_h264_)
    cam->start_stream_as(camera::RECORD_REQUESTER, camera::IMAGE_FORMAT_H264);
#endif
  return true;
}

#ifdef USE_CAMERA
void ScreenCapture::camera_h264_image_(const std::shared_ptr<camera::CameraImage> &image) {
  if (!this->cap_h264_ || image->get_format() != camera::IMAGE_FORMAT_H264 ||
      !image->was_requested_by(camera::RECORD_REQUESTER))
    return;
  this->cam_cap_key_ = image->is_key_frame();
  if (!this->camera_capture_frame(image->get_data_buffer(), image->get_data_length(), FrameFormat::H264,
                                  this->cap_w_, this->cap_h_, [image]() {})) {
    this->cap_need_key_.store(true, std::memory_order_release);
    camera::Camera::instance()->request_key_frame();
  }
}
#endif

void ScreenCapture::camera_record_stop() {
  if (this->cap_camera_)
    this->capture_end_("");
}

void ScreenCapture::camera_capture_end() {
  if (this->cam_cap_ == 1 && !this->cam_cap_busy_.load(std::memory_order_acquire)) {
    this->cam_cap_ = 0;
    this->shot_done_(false);
  }
  if (this->cap_camera_)
    this->capture_end_(R_CAMERA);
}

bool ScreenCapture::camera_capture_frame(const uint8_t *data, size_t length, FrameFormat format,
                                             uint16_t width, uint16_t height, std::function<void()> &&done) {
  this->host_->capture_wake();
  const bool recording = this->cap_camera_ && this->cast_state_ == CAST_RUNNING;
  const bool h264 = format == FrameFormat::H264;
  if (data == nullptr || (this->cam_cap_ == 0 && !recording) || format == FrameFormat::YUYV ||
      (h264 && (this->cam_cap_ != 0 || !this->cap_h264_)) || this->cam_cap_busy_.load(std::memory_order_acquire))
    return false;
  if (this->cam_cap_ == 0) {
    if (width != this->cap_w_ || height != this->cap_h_ || (this->cap_h264_ && !h264))
      return false;
    const uint32_t now = millis();
    if (!h264 && this->cam_cap_last_ != 0 && now - this->cam_cap_last_ < 1000u / this->cast_fps_)
      return false;
    this->cam_cap_last_ = now;
  }
  this->cam_cap_shot_ = this->cam_cap_ == 1;
  this->cam_cap_ = 0;
  this->cam_cap_data_ = data;
  this->cam_cap_len_ = length;
  this->cam_cap_format_ = format;
  this->cam_cap_w_ = width;
  this->cam_cap_h_ = height;
  this->cam_cap_done_ = std::move(done);
  this->cam_cap_busy_.store(true, std::memory_order_release);
  this->capture_notify_(CAPN_CAM);
  return true;
}

bool ScreenCapture::capture_cast_begin_(uint8_t target) {
  auto refuse = [this](const char *why) {
    ESP_LOGW(TAG, "No screencast: %s", why);
    this->cast_stopped_trigger_.trigger(std::string(why));
    return false;
  };
  this->cap_encoded_swaps_ = 0;
  this->cap_wjpeg_ = 0;
  this->cap_wlen_ = 0;
  this->cap_ejpeg_ready_ = false;
  this->cap_ticks_ = 0;
  this->cap_enc_errors_ = 0;
  this->cap_shot_wait_ = false;
  this->cap_jpeg_users_[0].store(0, std::memory_order_relaxed);
  this->cap_jpeg_users_[1].store(0, std::memory_order_relaxed);
  this->cap_batch_sinks_.store(0, std::memory_order_relaxed);
  this->cap_file_batch_.store(false, std::memory_order_relaxed);
  this->cap_stopping_.store(false, std::memory_order_relaxed);
  this->cap_drained_.store(false, std::memory_order_relaxed);
  this->cap_sinks_ = 0;
  this->cast_reason_.clear();

  // The sound: the microphone's format.
  this->cap_rate_ = 0;
  this->cap_channels_ = 0;
#ifdef USE_SCREEN_CAPTURE_MIC
  if (this->cap_pcm_ != nullptr) {
    const audio::AudioStreamInfo info = this->cap_mic_->get_audio_stream_info();
    if (info.get_bits_per_sample() == 16 && info.get_channels() != 0 && info.get_channels() <= 2) {
      this->cap_ring_ = this->host_->capture_ring(this->cap_pcm_, CAP_PCM_BYTES);
      if (this->cap_ring_ != nullptr) {
        this->cap_rate_ = info.get_sample_rate();
        this->cap_channels_ = info.get_channels();
      }
    }
  }
#endif
  if (!this->cap_avi_.begin(this->cap_w_, this->cap_h_, this->cast_fps_, this->cap_rate_, this->cap_channels_,
                            CAP_JPEG_ROOM, this->cap_index_, CAP_INDEX_BYTES, this->cap_h264_)) {
    this->capture_free_frames_();
    this->capture_free_rest_();
    this->cap_camera_ = false;
    return refuse(R_CAPTURE);
  }
  // The header first, for both: the file's and the stream's (the sizes come with the file's end).
  this->cap_avi_.build_header(this->cap_index_);

  this->cast_state_ = CAST_STARTING;
  this->cast_file_ = CF_NONE;
  if ((target & CAST_LOCAL) && !this->cast_file_open_())
    this->cast_file_ = CF_FAILED;
  this->cap_relay_.store(CAP_RELAY_NONE, std::memory_order_relaxed);
  if (target & CAST_RELAY) {
    this->cap_relay_.store(CAP_RELAY_ASKED, std::memory_order_release);
    this->cap_relay_asked_ = millis();
    this->cap_relay_asked_any_ = this->host_->capture_relay_ask(true);
    if (!this->cap_relay_asked_any_)
      this->cap_relay_.store(CAP_RELAY_FAILED, std::memory_order_release);
  }
  ESP_LOGI(TAG, "Screencast starts: %ux%u, %u fps, sound %s%s%s", this->cap_w_, this->cap_h_, this->cast_fps_,
           this->cap_rate_ != 0 ? "on" : "off", (target & CAST_LOCAL) ? ", file " : "",
           (target & CAST_LOCAL) ? this->cast_path_.c_str() : "");
  this->cap_callbacks_.call();
  return true;
}

void ScreenCapture::screencast_stop() { this->capture_end_(""); }

void ScreenCapture::capture_end_(const char *reason) {
  if (this->cast_state_ == CAST_IDLE || this->cast_state_ == CAST_ENDING)
    return;
  this->cast_reason_ = reason;
  if (this->cast_state_ == CAST_STARTING) {
    // Nothing went out yet: the file ends with its header.
    this->cast_state_ = CAST_ENDING;
    this->cap_drained_.store(true, std::memory_order_release);
  } else {
    this->cast_state_ = CAST_ENDING;
    esp_timer_stop(this->cap_timer_);
    this->cap_stopping_.store(true, std::memory_order_release);
    this->capture_notify_(CAPN_STOP);
  }
  ESP_LOGI(TAG, "Screencast ends%s%s", reason[0] != '\0' ? ": " : "", reason);
  this->cap_callbacks_.call();
}

bool ScreenCapture::give_back(uint8_t blocked) {
  if (this->cast_state_ == CAST_STARTING || this->cast_state_ == CAST_RUNNING) {
    this->capture_end_((blocked & CaptureHost::BLOCKED_DESKTOP)  ? R_DESKTOP
                       : (blocked & CaptureHost::BLOCKED_STREAM) ? R_STREAM
                                                                  : R_VIDEO);
  }
  return this->cap_frame_[0] == nullptr && this->cap_jpeg_[0] == nullptr;
}

void ScreenCapture::loop() {
  if (this->cam_cap_finished_.exchange(false, std::memory_order_acq_rel)) {
    auto done = std::move(this->cam_cap_done_);
    this->cam_cap_done_ = nullptr;
    this->cam_cap_busy_.store(false, std::memory_order_release);
    if (done)
      done();
  }
  // A screenshot's JPEG is there: into its file.
  if (this->shot_ready_.load(std::memory_order_acquire)) {
    this->shot_ready_.store(false, std::memory_order_relaxed);
    this->shot_write_();
  }
  if (this->cast_state_ == CAST_IDLE)
    return;

  // A video, a stream or a remote desktop takes the panel: the screencast ends.
  if (this->cast_state_ == CAST_STARTING || this->cast_state_ == CAST_RUNNING) {
    const char *why = nullptr;
    const uint8_t blocked = this->host_->capture_blocked();
    if (blocked & CaptureHost::BLOCKED_VIDEO)
      why = R_VIDEO;
    if (blocked & CaptureHost::BLOCKED_STREAM)
      why = R_STREAM;
    if (blocked & CaptureHost::BLOCKED_DESKTOP)
      why = R_DESKTOP;
    if (this->cap_broken_.load(std::memory_order_acquire))
      why = R_CAPTURE;
    if (why != nullptr)
      this->capture_end_(why);
  }

  // The media relay's answer, then the connection.
  const uint8_t relay = this->cap_relay_.load(std::memory_order_acquire);
  if (relay == CAP_RELAY_ASKED && millis() - this->cap_relay_asked_ > CAP_RELAY_WAIT_MS)
    this->cap_relay_.store(CAP_RELAY_FAILED, std::memory_order_release);

  if (this->cast_state_ == CAST_STARTING) {
    const bool file_waits = this->cast_file_ == CF_OPENING || this->cast_file_ == CF_HEADER ||
                            this->cast_file_ == CF_BUSY;
    const bool relay_waits = relay == CAP_RELAY_ASKED || relay == CAP_RELAY_CONNECTING;
    if (file_waits || relay_waits)
      return;
    const uint8_t sinks =
        (this->cast_file_ == CF_READY ? CAP_SINK_FILE : 0) | (relay == CAP_RELAY_READY ? CAP_SINK_RELAY : 0);
    this->cap_sinks_.store(sinks, std::memory_order_release);
    if (sinks == 0) {
      this->capture_end_(this->cast_file_ == CF_FAILED ? R_WRITE : R_RELAY);
    } else {
      if (relay == CAP_RELAY_FAILED)
        ESP_LOGW(TAG, "Screencast: %s, into the file only", R_RELAY);
      if (this->cast_file_ == CF_FAILED)
        ESP_LOGW(TAG, "Screencast: %s, to the media relay only", R_WRITE);
      // Running: LVGL's drawing, the sound and the tick.
      this->cast_state_ = CAST_RUNNING;
      this->cap_casting_.store(true, std::memory_order_release);
      if (!this->cap_camera_)
        this->lvgl_->set_frame_capture(&this->cap_sink_);
#ifdef USE_SCREEN_CAPTURE_MIC
      if (this->cap_ring_ != nullptr) {
        this->cap_ring_->reset();
        this->cap_ring_raw_.store(this->cap_ring_.get(), std::memory_order_release);
        this->cap_mic_->start();
        this->cap_mic_on_ = true;
      }
#endif
      esp_timer_start_periodic(this->cap_timer_, 1000000ull / this->cast_fps_);
      this->cap_callbacks_.call();
      this->cast_started_trigger_.trigger();
    }
  }

  if (this->cast_state_ == CAST_RUNNING) {
    // The media relay gone: the file goes on alone, else the screencast ends. The same the other way.
    const uint8_t sinks = this->cap_sinks_.load(std::memory_order_acquire);
    if ((sinks & CAP_SINK_RELAY) && relay == CAP_RELAY_FAILED) {
      this->cap_sinks_.fetch_and(static_cast<uint8_t>(~CAP_SINK_RELAY), std::memory_order_acq_rel);
      if ((sinks & ~CAP_SINK_RELAY) == 0) {
        this->capture_end_(R_RELAY_LOST);
      } else {
        ESP_LOGW(TAG, "Screencast: %s, into the file only", R_RELAY_LOST);
      }
    } else if ((sinks & CAP_SINK_FILE) && this->cast_file_ == CF_FAILED) {
      this->cap_sinks_.fetch_and(static_cast<uint8_t>(~CAP_SINK_FILE), std::memory_order_acq_rel);
      if ((sinks & ~CAP_SINK_FILE) == 0) {
        this->capture_end_(R_WRITE);
      } else {
        ESP_LOGW(TAG, "Screencast: %s, to the media relay only", R_WRITE);
      }
    }
  }

  if (this->cast_file_ == CF_READY && this->cap_file_batch_.load(std::memory_order_acquire)) {
    this->cap_file_batch_.store(false, std::memory_order_relaxed);
    this->cast_file_pending_ = true;
    this->cast_file_run_();
  }

  if (this->cast_state_ == CAST_ENDING && this->cap_drained_.load(std::memory_order_acquire) &&
      !this->cap_sync_busy_.load(std::memory_order_acquire)) {
    if (this->cap_casting_.load(std::memory_order_relaxed)) {
      this->cap_casting_.store(false, std::memory_order_release);
      if (!this->cap_camera_)
        this->lvgl_->set_frame_capture(nullptr);
#ifdef USE_SCREEN_CAPTURE_MIC
      this->cap_ring_raw_.store(nullptr, std::memory_order_release);
      if (this->cap_mic_on_) {
        this->cap_mic_->stop();
        this->cap_mic_on_ = false;
      }
#endif
    }
    // A screenshot of the screencast still writing a JPEG keeps them.
    if ((this->cap_frame_[0] != nullptr || this->cap_jpeg_[0] != nullptr) &&
        this->cap_jpeg_users_[0].load(std::memory_order_acquire) == 0 &&
        this->cap_jpeg_users_[1].load(std::memory_order_acquire) == 0 &&
        !this->cam_cap_busy_.load(std::memory_order_acquire)) {
      this->capture_free_frames_();
    }
    // The file's end: its indexes and header; the media relay's connection closed.
    if (this->cast_file_ == CF_READY && !this->cast_file_final_) {
      this->cast_file_final_ = true;
      this->cast_file_run_();
    }
    // The media relay: once its answer and the connection are settled, the connection closed and the
    // relay told.
    const uint8_t relay_now = this->cap_relay_.load(std::memory_order_acquire);
    if (relay_now == CAP_RELAY_READY || relay_now == CAP_RELAY_FAILED) {
      if (this->cap_tx_task_ != nullptr) {
        this->cap_tx_close_.store(true, std::memory_order_release);
        xTaskNotifyGive(this->cap_tx_task_);
      }
      if (this->cap_relay_asked_any_)
        this->host_->capture_relay_ask(false);
      this->cap_relay_asked_any_ = false;
      this->cap_relay_.store(CAP_RELAY_NONE, std::memory_order_release);
    }
    const bool file_done = this->cast_file_ == CF_NONE || this->cast_file_ == CF_DONE || this->cast_file_ == CF_FAILED;
    if (file_done && this->cap_frame_[0] == nullptr && this->cap_jpeg_[0] == nullptr &&
        this->cap_relay_.load(std::memory_order_acquire) == CAP_RELAY_NONE)
      this->capture_finished_();
  }
}

void ScreenCapture::capture_finished_() {
#ifdef USE_SCREEN_CAPTURE_MIC
  // The microphone's task may still be inside its write.
  while (this->cap_mic_in_.load(std::memory_order_acquire) != 0)
    delay(1);
#endif
  this->capture_free_rest_();
  // A screenshot that waited for the screencast's first JPEG.
  if (this->cap_shot_cast_.exchange(false, std::memory_order_acq_rel)) {
    this->shot_busy_ = false;
    this->shot_trigger_.trigger(std::string());
  }
  const bool written = this->cast_file_ == CF_DONE;
#ifdef USE_CAMERA
  if (this->cap_h264_ && camera::Camera::instance() != nullptr)
    camera::Camera::instance()->stop_stream_as(camera::RECORD_REQUESTER, camera::IMAGE_FORMAT_H264);
#endif
  this->cap_h264_ = false;
  this->cap_camera_ = false;
  this->cast_file_ = CF_NONE;
  this->cast_file_final_ = false;
  this->cast_file_pending_ = false;
  this->cast_state_ = CAST_IDLE;
  if (written) {
    ESP_LOGI(TAG, "Screencast %s: %" PRIu32 " pictures, %" PRIu64 " bytes", this->cast_path_.c_str(),
             this->cap_avi_.pictures(), this->cap_avi_.file_size());
  }
  if (this->cap_enc_errors_ != 0)
    ESP_LOGW(TAG, "Screencast: %" PRIu32 " pictures not encoded (larger than %zu bytes as JPEG?)",
             this->cap_enc_errors_, CAP_JPEG_ROOM);
  this->cap_callbacks_.call();
  this->cast_stopped_trigger_.trigger(this->cast_reason_);
}

//==================================================================================================
// Main loop: the screencast's file
//==================================================================================================

bool ScreenCapture::cast_file_open_() {
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (storage::global_storage_registry == nullptr || worker == nullptr)
    return false;
  const char *dir_rel = nullptr;
  storage::PathStorage *dir = storage::global_storage_registry->resolve_path(this->cast_dir_.c_str(), &dir_rel);
  if (dir == nullptr)
    return false;
  if (dir_rel != nullptr && dir_rel[0] != '\0' && !worker->has_active_task_io(dir))
    dir->mkdir(dir_rel);  // already there: an error, and nothing to do
  this->cast_path_ = this->capture_name_(this->cast_dir_, this->cast_prefix_, ".avi");
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->cast_path_.c_str(), &rel);
  if (ps == nullptr)
    return false;
  this->cast_file_ = CF_OPENING;
  this->cast_file_final_ = false;
  this->cast_file_pending_ = false;
  const storage::StorageError err =
      worker->begin_write(ps, rel, &this->cast_handle_, [this](storage::StorageError e) {
        this->host_->capture_defer([this, e]() {
          if (e != storage::StorageError::STORAGE_ERROR_OK) {
            ESP_LOGW(TAG, "%s not opened: %s", this->cast_path_.c_str(), storage::error_to_string(e));
            this->cast_file_ = CF_FAILED;
            return;
          }
          this->cast_file_ = CF_HEADER;
          this->cast_file_run_();
        });
      });
  return err == storage::StorageError::STORAGE_ERROR_OK;
}

void ScreenCapture::cast_file_op_(storage::StorageError err) {
  if (err == storage::StorageError::STORAGE_ERROR_OK)
    return;
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY) {
    // The worker is not ready for it yet: again in the next loop pass.
    this->host_->capture_defer([this]() {
      const storage::StorageError again =
          this->cast_op_seek_
              ? storage::global_storage_worker->seek(this->cast_handle_, static_cast<int64_t>(this->cast_op_pos_),
                                                     storage::SeekMode::SEEK_MODE_SET,
                                                     [this](storage::StorageError e) { this->cast_file_result_(e); })
              : storage::global_storage_worker->write_chunk(
                    this->cast_handle_, this->cast_op_data_, this->cast_op_len_,
                    [this](storage::StorageError e) { this->cast_file_result_(e); });
      this->cast_file_op_(again);
    });
    return;
  }
  this->cast_file_fail_();
}

void ScreenCapture::cast_file_result_(storage::StorageError err) {
  this->host_->capture_defer([this, err]() {
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      ESP_LOGW(TAG, "%s: %s", this->cast_path_.c_str(), storage::error_to_string(err));
      this->cast_file_fail_();
      return;
    }
    this->cast_file_ = this->cast_file_next_;
    this->cast_file_run_();
  });
}

void ScreenCapture::cast_file_write_(const uint8_t *data, size_t len, uint8_t next) {
  this->cast_file_ = CF_BUSY;
  this->cast_file_next_ = next;
  this->cast_op_seek_ = false;
  this->cast_op_data_ = data;
  this->cast_op_len_ = len;
  this->cast_file_op_(storage::global_storage_worker->write_chunk(
      this->cast_handle_, data, len, [this](storage::StorageError e) { this->cast_file_result_(e); }));
}

void ScreenCapture::cast_file_seek_(uint64_t pos, uint8_t next) {
  this->cast_file_ = CF_BUSY;
  this->cast_file_next_ = next;
  this->cast_op_seek_ = true;
  this->cast_op_pos_ = pos;
  this->cast_file_op_(storage::global_storage_worker->seek(this->cast_handle_, static_cast<int64_t>(pos),
                                                           storage::SeekMode::SEEK_MODE_SET,
                                                           [this](storage::StorageError e) { this->cast_file_result_(e); }));
}

void ScreenCapture::cast_file_fail_() {
  this->cast_file_ = CF_FAILED;
  if (this->cast_file_pending_) {
    this->cast_file_pending_ = false;
    this->capture_sink_done_(CAP_SINK_FILE);
  }
  storage::global_storage_worker->end_write(this->cast_handle_, [](storage::StorageError) {});
}

// The next step of the file: the header, the batches, a segment's end, the file's end.
void ScreenCapture::cast_file_run_() {
  switch (this->cast_file_) {
    case CF_HEADER:
      this->cast_file_write_(this->cap_index_, this->cap_avi_.header_size(), CF_READY);
      return;
    case CF_READY:
      if (this->cast_file_pending_) {
        if (this->cap_avi_.fits(this->cap_batch_)) {
          this->cap_avi_.add(this->cap_batch_);
          this->cast_file_write_(this->cap_batch_data_, this->cap_batch_len_, CF_BATCH);
          return;
        }
        if (this->cap_avi_.segments_full()) {
          // The batch is not written; the file ends here (no batch after this one).
          this->capture_end_(R_FULL);
          this->cast_file_final_ = true;
          this->cast_file_pending_ = false;
          this->capture_sink_done_(CAP_SINK_FILE);
        }
      } else if (!this->cast_file_final_) {
        return;
      }
      // This segment ends: its indexes.
      this->cast_file_ = CF_IX_VIDEO;
      this->cast_file_run_();
      return;
    case CF_BATCH:
      this->cast_file_pending_ = false;
      this->cast_file_ = CF_READY;
      this->capture_sink_done_(CAP_SINK_FILE);
      if (this->cast_file_final_)
        this->cast_file_run_();
      return;
    case CF_IX_VIDEO: {
      size_t len = 0;
      const uint8_t *ix = this->cap_avi_.close_index(0, &len);
      this->cast_file_write_(ix, len, CF_IX_AUDIO);
      return;
    }
    case CF_IX_AUDIO: {
      size_t len = 0;
      const uint8_t *ix = this->cap_avi_.close_index(1, &len);
      if (len == 0) {
        this->cast_file_ = CF_SEEK_HEADER;
        this->cast_file_run_();
        return;
      }
      this->cast_file_write_(ix, len, CF_SEEK_HEADER);
      return;
    }
    case CF_SEEK_HEADER:
      this->cap_avi_.close_sizes();
      this->cast_file_seek_(0, CF_HEADER_AGAIN);
      return;
    case CF_HEADER_AGAIN:
      this->cap_avi_.build_header(this->cap_avi_.index_free());
      this->cast_file_write_(this->cap_avi_.index_free(), this->cap_avi_.header_size(),
                             this->cap_avi_.first_segment() ? CF_SEEK_END : CF_SEEK_PATCH);
      return;
    case CF_SEEK_PATCH:
      this->cast_file_seek_(this->cap_avi_.segment_start(), CF_PATCH);
      return;
    case CF_PATCH: {
      uint8_t *patch = this->cap_avi_.index_free() + this->cap_avi_.header_size();
      this->cap_avi_.build_segment_patch(patch);
      this->cast_file_write_(patch, avi::CastAvi::SEGMENT_START, CF_SEEK_END);
      return;
    }
    case CF_SEEK_END:
      if (this->cast_file_final_ && !this->cast_file_pending_) {
        this->cast_file_ = CF_CLOSING;
        storage::global_storage_worker->end_write(this->cast_handle_, [this](storage::StorageError e) {
          this->host_->capture_defer([this, e]() {
            this->cast_file_ = e == storage::StorageError::STORAGE_ERROR_OK ? CF_DONE : CF_FAILED;
          });
        });
        return;
      }
      this->cast_file_seek_(this->cap_avi_.file_size(), CF_SEGMENT);
      return;
    case CF_SEGMENT:
      this->cap_avi_.next_segment(this->cap_avi_.index_free());
      this->cast_file_write_(this->cap_avi_.index_free(), avi::CastAvi::SEGMENT_START, CF_READY);
      return;
    default:
      return;
  }
}

//==================================================================================================
// media relay
//==================================================================================================

// Channel task: the media relay takes the screencast on a port of its own (start), or lets it go.
void ScreenCapture::cast_relay_task_(bool start) {
  const std::string payload = json::build_json([&](JsonObject root) {
    root["port"] = this->host_->capture_stream_port();
    root["action"] = start ? "start" : "stop";
    if (start) {
      root["fps"] = this->cast_fps_;
      root["width"] = this->cap_w_;
      root["height"] = this->cap_h_;
      root["sound"] = this->cap_rate_ != 0;
    }
  });
  char *body = nullptr;
  size_t len = 0;
  bool ok = this->host_->capture_http("/cast", payload.c_str(), start ? &body : nullptr, start ? &len : nullptr);
  if (!start) {
    if (body != nullptr)
      heap_caps_free(body);
    return;
  }
  uint16_t port = 0;
  if (ok) {
    ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
      port = root["port"] | 0;
      const char *token = root["token"] | "";
      std::snprintf(this->cap_relay_token_, sizeof(this->cap_relay_token_), "%s", token);
      return true;
    });
  }
  if (body != nullptr)
    heap_caps_free(body);
  if (this->cap_relay_.load(std::memory_order_acquire) != CAP_RELAY_ASKED)
    return;  // given up meanwhile
  if (!ok || port == 0) {
    this->cap_relay_.store(CAP_RELAY_FAILED, std::memory_order_release);
    App.wake_loop_threadsafe();
    return;
  }
  this->cap_relay_port_.store(port, std::memory_order_relaxed);
  if (this->cap_tx_task_ == nullptr &&
      !create_psram_task(&ScreenCapture::cast_tx_entry_, "screen_cast_tx", CAP_TASK_STACK, this, 3,
                         &this->cap_tx_task_, 0)) {
    this->cap_tx_task_ = nullptr;
    this->cap_relay_.store(CAP_RELAY_FAILED, std::memory_order_release);
    App.wake_loop_threadsafe();
    return;
  }
  this->cap_relay_.store(CAP_RELAY_CONNECTING, std::memory_order_release);
  this->cap_tx_connect_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->cap_tx_task_);
}

void ScreenCapture::cast_tx_entry_(void *param) { static_cast<ScreenCapture *>(param)->cast_tx_loop_(); }

bool ScreenCapture::cast_tx_send_(const uint8_t *data, size_t len) {
  while (len > 0) {
    const ssize_t sent = ::send(this->cap_sock_, data, len, 0);
    if (sent <= 0)
      return false;
    data += sent;
    len -= static_cast<size_t>(sent);
  }
  return true;
}

void ScreenCapture::cast_tx_close_() {
  if (this->cap_sock_ >= 0) {
    ::shutdown(this->cap_sock_, SHUT_RDWR);
    ::close(this->cap_sock_);
  }
  this->cap_sock_ = -1;
}

// The cast sender task: connects to the media relay, sends the header and then each batch.
void ScreenCapture::cast_tx_loop_() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // The last screencast's connection first, before a new one's.
    if (this->cap_tx_close_.exchange(false, std::memory_order_acq_rel))
      this->cast_tx_close_();
    if (this->cap_tx_connect_.exchange(false, std::memory_order_acq_rel)) {
      bool ok = false;
      // The media relay's host: the relay link's URL's.
      std::string host = this->host_->capture_relay_url();
      const size_t scheme = host.find("://");
      if (scheme != std::string::npos)
        host.erase(0, scheme + 3);
      host = host.substr(0, host.find_first_of(":/"));
      struct addrinfo hints {};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      struct addrinfo *found = nullptr;
      char port_text[6];
      std::snprintf(port_text, sizeof(port_text), "%u", this->cap_relay_port_.load(std::memory_order_relaxed));
      if (::getaddrinfo(host.c_str(), port_text, &hints, &found) == 0 && found != nullptr) {
        this->cap_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (this->cap_sock_ >= 0) {
          struct timeval tv = {.tv_sec = CAP_TX_TIMEOUT_MS / 1000, .tv_usec = 0};
          ::setsockopt(this->cap_sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
          int one = 1;
          ::setsockopt(this->cap_sock_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          // Connected within the timeout, else given up.
          const int flags = ::fcntl(this->cap_sock_, F_GETFL, 0);
          ::fcntl(this->cap_sock_, F_SETFL, flags | O_NONBLOCK);
          bool connected = ::connect(this->cap_sock_, found->ai_addr, found->ai_addrlen) == 0;
          if (!connected && errno == EINPROGRESS) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(this->cap_sock_, &writable);
            int error = 0;
            socklen_t error_len = sizeof(error);
            connected = ::select(this->cap_sock_ + 1, nullptr, &writable, nullptr, &tv) == 1 &&
                        ::getsockopt(this->cap_sock_, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0;
          }
          ::fcntl(this->cap_sock_, F_SETFL, flags);
          if (connected) {
            char line[64];
            const int n = std::snprintf(line, sizeof(line), "MEDIACAST %s\n", this->cap_relay_token_);
            ok = this->cast_tx_send_(reinterpret_cast<const uint8_t *>(line), static_cast<size_t>(n)) &&
                 this->cast_tx_send_(this->cap_index_, this->cap_avi_.header_size());
          }
        }
        ::freeaddrinfo(found);
      }
      if (!ok)
        this->cast_tx_close_();
      this->cap_relay_.store(ok ? CAP_RELAY_READY : CAP_RELAY_FAILED, std::memory_order_release);
      App.wake_loop_threadsafe();
    }
    if (this->cap_tx_send_.exchange(false, std::memory_order_acq_rel)) {
      if (this->cap_sock_ >= 0 && !this->cast_tx_send_(this->cap_batch_data_, this->cap_batch_len_)) {
        this->cast_tx_close_();
        this->cap_relay_.store(CAP_RELAY_FAILED, std::memory_order_release);
        App.wake_loop_threadsafe();
      }
      this->capture_sink_done_(CAP_SINK_RELAY);
    }
  }
}

}  // namespace esphome::screen_capture

#endif
