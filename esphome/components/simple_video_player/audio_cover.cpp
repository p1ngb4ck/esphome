#include "simple_video_player.h"

#ifdef SVP_AUDIO_COVER

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_cache.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.cover";

static constexpr size_t DMA_ALIGN = PsramPool::ALIGN;  // whole cache lines, as the JPEG unit needs

static constexpr size_t align_up(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }

// The decoder writes whole MCUs (esp_driver_jpeg jpeg_parse_marker.c): 16 pixels wide for 4:2:2
// and 4:2:0, 16 high for 4:2:0, else 8. RGB565.
static size_t decoded_size(const jpeg_decode_picture_info_t &info, uint32_t *padded_w) {
  const bool wide = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 || info.sample_method == JPEG_DOWN_SAMPLING_YUV420;
  const uint32_t pw = align_up(info.width, wide ? 16 : 8);
  const uint32_t ph = align_up(info.height, info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8);
  if (padded_w != nullptr)
    *padded_w = pw;
  return static_cast<size_t>(pw) * ph * 2;
}

void SimpleVideoPlayer::on_svp_cover() {
  this->cover_wanted_ = true;
  this->cover_clear_ = false;
}

// Setup: the canvases' sizes (the configuration's) and what their buffers take of the pool once cover art shows.
size_t SimpleVideoPlayer::cover_bytes_() {
  size_t total = 0;
  for (size_t i = 0; i < this->cover_view_count_; i++) {
    CoverView &view = this->cover_views_[i];
    // The size set in the configuration (a hidden canvas may not be laid out yet).
    view.w = lv_obj_get_style_width(view.canvas, LV_PART_MAIN);
    view.h = lv_obj_get_style_height(view.canvas, LV_PART_MAIN);
    if (LV_COORD_IS_SPEC(view.w) || LV_COORD_IS_SPEC(view.h)) {
      lv_obj_update_layout(view.canvas);
      view.w = lv_obj_get_width(view.canvas);
      view.h = lv_obj_get_height(view.canvas);
    }
    // Room for the decoder's whole MCUs at the canvas' size.
    view.capacity = view.w > 0 && view.h > 0 ? align_up(align_up(view.w, 16) * align_up(view.h, 16) * 2, DMA_ALIGN) : 0;
    total += view.capacity;
  }
  return total + align_up(COVER_FILE_MAX + 16, DMA_ALIGN);
}

// From the pool; when it is full, the video's idle buffers make room (taken again by the next video).
uint8_t *SimpleVideoPlayer::cover_alloc_(size_t size) {
  uint8_t *piece = this->pool_.alloc(size);
  if (piece == nullptr && this->release_media_())
    piece = this->pool_.alloc(size);
  return piece;
}

// Main loop, no video: the buffers from the pool while cover art is wanted (a video takes them back,
// release_cover_buffers_()).
bool SimpleVideoPlayer::cover_buffers_() {
  bool ok = true;
  for (size_t i = 0; ok && i < this->cover_view_count_; i++) {
    CoverView &view = this->cover_views_[i];
    if (view.data == nullptr && view.capacity != 0)
      view.data = this->cover_alloc_(view.capacity);
    ok = view.data != nullptr;
  }
  if (ok && this->cover_file_ == nullptr)
    this->cover_file_ = this->cover_alloc_(align_up(COVER_FILE_MAX + 16, DMA_ALIGN));
  if (ok && this->cover_file_ != nullptr)
    return true;
  ESP_LOGE(TAG, "No room in the PSRAM pool for the cover art");
  this->release_cover_buffers_();
  return false;
}

// Main loop, nothing fetched or decoded meanwhile: the cover art's buffers back to the pool (a video
// wants it). The canvases let go first; what showed is fetched again afterwards.
void SimpleVideoPlayer::release_cover_buffers_() {
  if (this->cover_shown_)
    this->cover_wanted_ = true;
  if (this->lvgl_component_ == nullptr || this->lvgl_component_->is_paused()) {
    this->cover_detach_pending_ = this->cover_detach_pending_ || this->cover_shown_;  // once LVGL runs
  } else {
    this->cover_clear_views_();
  }
  this->cover_shown_ = false;
  for (size_t i = 0; i < this->cover_view_count_; i++) {
    this->pool_.free(this->cover_views_[i].data);
    this->cover_views_[i].data = nullptr;
  }
  this->pool_.free(this->cover_file_);
  this->cover_file_ = nullptr;
  this->cover_step_ = CoverStep::IDLE;
  this->cover_view_ = 0;
}

// LVGL running: the canvas shows nothing (no longer reads the buffer).
void SimpleVideoPlayer::cover_detach_(CoverView &view) {
  if (view.shown_w == 0)
    return;
  lv_image_set_src(view.canvas, nullptr);
  view.shown_w = 0;
  view.shown_h = 0;
}

void SimpleVideoPlayer::cover_clear_views_() {
  for (size_t i = 0; i < this->cover_view_count_; i++)
    this->cover_detach_(this->cover_views_[i]);
  this->cover_shown_ = false;
}

void SimpleVideoPlayer::cover_loop_() {
  if (this->cover_view_count_ == 0 || !this->pic_ready_ || this->channel_q_ == nullptr)
    return;
  // Nothing of LVGL while it is paused: everything waits for it.
  if (this->lvgl_component_ == nullptr || this->lvgl_component_->is_paused())
    return;
  if (this->cover_detach_pending_) {
    // Given back to the pool while LVGL was paused: the canvases still point at it.
    this->cover_detach_pending_ = false;
    for (size_t i = 0; i < this->cover_view_count_; i++)
      this->cover_detach_(this->cover_views_[i]);
  }
  switch (this->cover_step_) {
    case CoverStep::IDLE:
      if (this->cover_wanted_) {
        // Not while a video plays or takes the pool: the buffers are its then.
        if (this->pictures_held_() || this->pool_reclaim_req_.load(std::memory_order_acquire))
          return;
        if (!this->cover_buffers_()) {
          this->cover_wanted_ = false;
          return;
        }
        this->cover_view_ = 0;
      } else if (this->cover_clear_) {
        this->cover_clear_ = false;
        this->cover_clear_views_();
        return;
      } else if (this->cover_view_ == 0 || this->cover_view_ >= this->cover_view_count_) {
        return;  // all views done
      }
      this->cover_wanted_ = false;
      this->cover_fetch_w_ = this->cover_views_[this->cover_view_].w;
      this->cover_fetch_h_ = this->cover_views_[this->cover_view_].h;
      this->cover_fetched_.store(false, std::memory_order_relaxed);
      if (!this->channel_request_(CHANNEL_CMD_COVER, 0)) {
        this->cover_wanted_ = this->cover_view_ == 0;  // the task's queue is full: again next loop
        return;
      }
      this->cover_step_ = CoverStep::FETCHING;
      return;

    case CoverStep::FETCHING: {
      if (!this->cover_fetched_.load(std::memory_order_acquire))
        return;
      if (this->cover_wanted_ || this->cover_clear_) {
        this->cover_step_ = CoverStep::IDLE;  // newer: from the first view again, or none
        return;
      }
      if (!this->cover_fetch_ok_) {
        this->cover_step_ = CoverStep::IDLE;
        this->cover_view_ = 0;
        return;  // not reached: what shows stays
      }
      if (this->cover_len_ == 0) {
        this->cover_step_ = CoverStep::IDLE;
        this->cover_view_ = 0;
        this->cover_clear_views_();  // none
        return;
      }
      CoverView &view = this->cover_views_[this->cover_view_];
      jpeg_decode_picture_info_t info{};
      if (this->hw_jpeg_decoder_ == nullptr ||
          jpeg_decoder_get_info(this->cover_file_, static_cast<uint32_t>(this->cover_len_), &info) != ESP_OK ||
          info.width == 0 || info.height == 0 || static_cast<int32_t>(info.width) > view.w ||
          static_cast<int32_t>(info.height) > view.h || decoded_size(info, nullptr) > view.capacity) {
        ESP_LOGW(TAG, "Cover art for %" PRId32 "x%" PRId32 ": not a JPEG that fits", view.w, view.h);
        this->cover_detach_(view);  // not the last track's
        this->cover_step_ = CoverStep::IDLE;
        this->cover_view_++;
        return;
      }
      // The JPEG unit: not while a video plays or takes the pool, nor while a picture decodes.
      if (this->pictures_held_() || this->pool_reclaim_req_.load(std::memory_order_acquire) ||
          this->pic_read_.decoding)
        return;
      this->cover_detach_(view);  // LVGL lets go of the buffer before it is written
      this->cover_decoded_.store(false, std::memory_order_relaxed);
      this->cover_job_.store(true, std::memory_order_release);
      this->cover_step_ = CoverStep::DECODING;
      xTaskNotifyGive(this->pic_task_);
      return;
    }

    case CoverStep::DECODING: {
      if (!this->cover_decoded_.load(std::memory_order_acquire))
        return;
      this->cover_step_ = CoverStep::IDLE;
      CoverView &view = this->cover_views_[this->cover_view_++];
      if (this->cover_wanted_ || this->cover_clear_ || !this->cover_decode_ok_)
        return;
      lv_draw_buf_init(&view.buf, view.shown_w, view.shown_h, LV_COLOR_FORMAT_RGB565, view.stride, view.data,
                       view.capacity);
      lv_canvas_set_draw_buf(view.canvas, &view.buf);
      lv_obj_invalidate(view.canvas);
      this->cover_shown_ = true;
      return;
    }
  }
}

// Channel task: the cover art for the view asked for, into cover_file_; empty: none.
void SimpleVideoPlayer::cover_task_() {
  char path[96];
  snprintf(path, sizeof(path), "/cover?port=%u&w=%d&h=%d&max=%u", static_cast<unsigned>(this->stream_port_),
           static_cast<int>(this->cover_fetch_w_), static_cast<int>(this->cover_fetch_h_),
           static_cast<unsigned>(COVER_FILE_MAX));
  size_t len = 0;
  const bool ok = this->channel_http_(path, nullptr, nullptr, &len, this->cover_file_, COVER_FILE_MAX + 1);
  this->cover_fetch_ok_ = ok && len <= COVER_FILE_MAX;
  this->cover_len_ = this->cover_fetch_ok_ ? len : 0;
  this->cover_fetched_.store(true, std::memory_order_release);
}

// Picture task: cover_file_ into the view's buffer at the JPEG's own size.
void SimpleVideoPlayer::cover_decode_() {
  this->cover_decode_ok_ = false;
  CoverView &view = this->cover_views_[this->cover_view_];
  jpeg_decode_picture_info_t info{};
  if (jpeg_decoder_get_info(this->cover_file_, static_cast<uint32_t>(this->cover_len_), &info) != ESP_OK)
    return;
  uint32_t pw = 0;
  decoded_size(info, &pw);
  jpeg_decode_cfg_t cfg{};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // little-endian RGB565, as LVGL's
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  // Lines the CPU wrote go out first, or one written back later would land on the picture.
  esp_cache_msync(view.data, view.capacity, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  uint32_t out = 0;
  if (jpeg_decoder_process(this->hw_jpeg_decoder_, &cfg, this->cover_file_,
                           static_cast<uint32_t>(align_up(this->cover_len_, 16)), view.data,
                           static_cast<uint32_t>(view.capacity), &out) != ESP_OK ||
      out == 0) {
    ESP_LOGW(TAG, "Cover art: decoding failed");
    return;
  }
  view.shown_w = static_cast<int32_t>(info.width);
  view.shown_h = static_cast<int32_t>(info.height);
  view.stride = pw * 2;
  this->cover_decode_ok_ = true;
}

}  // namespace esphome::simple_video_player

#endif  // SVP_AUDIO_COVER
