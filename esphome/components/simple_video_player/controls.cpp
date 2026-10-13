#include "simple_video_player.h"

#ifdef SVP_CONTROLS

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "simple_video_player.controls";

static bool inside(const lv_area_t &area, int32_t x, int32_t y) {
  return x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2;
}

void ControlsTouchListener::update(const touchscreen::TouchPoints_t &points) {
  if (points.empty())
    return;
  this->parent_->controls_touch(true, points[0].x, points[0].y);
}

void ControlsTouchListener::release() { this->parent_->controls_touch(false, 0, 0); }

bool SimpleVideoPlayer::controls_active_() const {
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  return this->ctl_widget_ != nullptr && !this->ctl_failed_ && this->dsi_ != nullptr && this->dsi_lvgl_paused_ &&
         (state == PlayerState::PLAYING || state == PlayerState::PAUSED) && !this->is_network_session();
}

void SimpleVideoPlayer::show_controls() {
  if (!this->controls_active_())
    return;
  this->ctl_visible_ = true;
  this->ctl_dirty_ = true;
  this->ctl_last_input_ms_ = millis();
}

void SimpleVideoPlayer::hide_controls() {
  this->ctl_visible_ = false;
  this->ctl_dragging_ = false;
  this->ctl_shown_.store(nullptr, std::memory_order_release);
}

float SimpleVideoPlayer::controls_fraction_(int32_t x) const {
  const int32_t width = lv_area_get_width(&this->ctl_seek_area_);
  if (width <= 1)
    return 0.0f;
  const float fraction = static_cast<float>(x - this->ctl_seek_area_.x1) / static_cast<float>(width - 1);
  return fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
}

void SimpleVideoPlayer::controls_touch(bool pressed, int32_t x, int32_t y) {
  if (!this->controls_active_()) {
    this->ctl_pressed_ = false;
    this->ctl_dragging_ = false;
    return;
  }
  this->ctl_last_input_ms_ = millis();
  if (pressed) {
    // Touches come in panel coordinates; the controls are laid out in LVGL's.
    this->lvgl_component_->rotate_coordinates(x, y);
    if (!this->ctl_pressed_) {
      this->ctl_pressed_ = true;
      this->ctl_dragging_ =
          this->ctl_visible_ && this->ctl_seek_ != nullptr && this->can_seek() && inside(this->ctl_seek_area_, x, y);
    }
    this->ctl_x_ = x;
    this->ctl_y_ = y;
    if (this->ctl_dragging_) {
      this->ctl_drag_fraction_ = this->controls_fraction_(x);
      this->ctl_dirty_ = true;
    }
    return;
  }
  if (!this->ctl_pressed_)
    return;
  this->ctl_pressed_ = false;
  if (this->ctl_dragging_) {
    this->ctl_dragging_ = false;
    this->seek(this->ctl_drag_fraction_ * this->get_duration());
    this->ctl_dirty_ = true;
    return;
  }
  if (!this->ctl_visible_) {
    this->show_controls();
    return;
  }
  if (!inside(this->ctl_area_, this->ctl_x_, this->ctl_y_)) {
    // Beside the controls: hidden again (they stay while paused).
    if (this->is_playing())
      this->hide_controls();
    return;
  }
  // The widget under the finger, as LVGL's own input would find it; LVGL does not run meanwhile.
  lv_obj_set_hidden(this->ctl_widget_, false);
  lv_point_t search{this->ctl_x_, this->ctl_y_};
  lv_obj_t *target = lv_indev_search_obj(this->ctl_widget_, &search);
  lv_obj_set_hidden(this->ctl_widget_, true);
  if (target != nullptr && target != this->ctl_widget_)
    lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
  this->ctl_dirty_ = true;
}

void SimpleVideoPlayer::controls_loop_() {
  if (this->ctl_widget_ == nullptr)
    return;
  if (!this->controls_active_()) {
    if (this->ctl_visible_ || this->ctl_shown_.load(std::memory_order_relaxed) != nullptr)
      this->hide_controls();
    this->ctl_pressed_ = false;
    this->ctl_last_state_ = PlayerState::STOPPED;
    return;
  }
  if (!this->ctl_visible_)
    return;
  const uint32_t now = millis();
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  if (state == PlayerState::PLAYING && !this->ctl_pressed_ &&
      now - this->ctl_last_input_ms_ > this->ctl_hide_after_ms_) {
    this->hide_controls();
    return;
  }
  const uint32_t second = static_cast<uint32_t>(this->get_position());
  const bool changed = this->ctl_dirty_ || state != this->ctl_last_state_ || second != this->ctl_last_second_ ||
                       this->repeat_ != this->ctl_last_repeat_;
  static constexpr uint32_t MIN_RENDER_INTERVAL_MS = 100;
  if (!changed || now - this->ctl_last_render_ms_ < MIN_RENDER_INTERVAL_MS)
    return;
  this->ctl_dirty_ = false;
  this->ctl_last_state_ = state;
  this->ctl_last_second_ = second;
  this->ctl_last_repeat_ = this->repeat_;
  this->ctl_last_render_ms_ = now;
  this->render_controls_();
}

static void format_time(char *out, size_t size, float seconds) {
  const uint32_t total = seconds > 0.0f ? static_cast<uint32_t>(seconds) : 0;
  if (total >= 3600) {
    snprintf(out, size, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, total / 3600, total / 60 % 60, total % 60);
  } else {
    snprintf(out, size, "%" PRIu32 ":%02" PRIu32, total / 60, total % 60);
  }
}

static void set_label(lv_obj_t *obj, const char *text) {
  if (obj == nullptr)
    return;
  if (!lv_obj_check_type(obj, &lv_label_class)) {
    // A button with a text: its label is the first child.
    obj = lv_obj_get_child_count(obj) != 0 ? lv_obj_get_child(obj, 0) : nullptr;
    if (obj == nullptr || !lv_obj_check_type(obj, &lv_label_class))
      return;
  }
  if (strcmp(lv_label_get_text(obj), text) != 0)
    lv_label_set_text(obj, text);
}

void SimpleVideoPlayer::render_controls_() {
  // What the widgets show.
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  const float duration = this->get_duration();
  const float position = this->ctl_dragging_ ? this->ctl_drag_fraction_ * duration : this->get_position();
  if (this->ctl_title_ != nullptr) {
    const size_t slash = this->video_path_.rfind('/');
    set_label(this->ctl_title_, this->video_path_.c_str() + (slash == std::string::npos ? 0 : slash + 1));
  }
  set_label(this->ctl_play_pause_, state == PlayerState::PAUSED ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  set_label(this->ctl_repeat_, this->repeat_ == RepeatMode::FILE       ? LV_SYMBOL_LOOP " 1"
                               : this->repeat_ == RepeatMode::PLAYLIST ? LV_SYMBOL_LOOP " all"
                                                                       : LV_SYMBOL_LOOP " off");
  if (this->ctl_position_ != nullptr) {
    char pos[16];
    char len[16];
    char text[40];
    format_time(pos, sizeof(pos), position);
    format_time(len, sizeof(len), duration);
    snprintf(text, sizeof(text), "%s / %s", pos, len);
    set_label(this->ctl_position_, text);
  }
#if LV_USE_SLIDER
  if (this->ctl_seek_ != nullptr && lv_obj_check_type(this->ctl_seek_, &lv_slider_class)) {
    lv_slider_set_range(this->ctl_seek_, 0, 1000);
    lv_slider_set_value(this->ctl_seek_, duration > 0.0f ? static_cast<int32_t>(position * 1000.0f / duration) : 0,
                        LV_ANIM_OFF);
  }
#endif

  // Rendered off screen: LVGL is paused, so this is the only drawing it does meanwhile.
  const uint8_t bpp = this->dsi_fb_bpp_;
  const lv_color_format_t cf = bpp == 3 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565;
  lv_obj_set_hidden(this->ctl_widget_, false);
  lv_obj_update_layout(this->ctl_widget_);
  bool ok = this->ctl_snapshot_ != nullptr &&
            lv_snapshot_take_to_draw_buf(this->ctl_widget_, cf, this->ctl_snapshot_) == LV_RESULT_OK;
  if (!ok) {
    // First time, or the controls grew: a buffer of their size.
    if (this->ctl_snapshot_ != nullptr)
      lv_draw_buf_destroy(this->ctl_snapshot_);
    this->ctl_snapshot_ = lv_snapshot_create_draw_buf(this->ctl_widget_, cf);
    ok = this->ctl_snapshot_ != nullptr &&
         lv_snapshot_take_to_draw_buf(this->ctl_widget_, cf, this->ctl_snapshot_) == LV_RESULT_OK;
  }
  lv_area_t area;
  lv_obj_get_coords(this->ctl_widget_, &area);
  const int32_t ext = lv_obj_get_ext_draw_size(this->ctl_widget_);
  lv_obj_get_coords(this->ctl_widget_, &this->ctl_area_);
  if (this->ctl_seek_ != nullptr)
    lv_obj_get_coords(this->ctl_seek_, &this->ctl_seek_area_);
  lv_obj_set_hidden(this->ctl_widget_, true);
  if (!ok) {
    ESP_LOGE(TAG, "Could not render the controls -- disabled");
    this->ctl_failed_ = true;
    this->hide_controls();
    return;
  }
  lv_area_increase(&area, ext, ext);

  // The part on the screen, in LVGL coordinates, then where it lies on the panel. Same mapping as
  // LvglComponent's rotated flush: 90 puts LVGL's (x, y) at the panel's (W - 1 - y, x).
  const int32_t pw = this->dsi_out_w_;
  const int32_t ph = this->dsi_out_h_;
  const display::DisplayRotation rotation = this->lvgl_component_->get_rotation();
  const bool swapped =
      rotation == display::DISPLAY_ROTATION_90_DEGREES || rotation == display::DISPLAY_ROTATION_270_DEGREES;
  const int32_t lw = swapped ? ph : pw;
  const int32_t lh = swapped ? pw : ph;
  const int32_t cx0 = std::max<int32_t>(area.x1, 0);
  const int32_t cy0 = std::max<int32_t>(area.y1, 0);
  const int32_t cx1 = std::min<int32_t>(area.x1 + static_cast<int32_t>(this->ctl_snapshot_->header.w), lw);
  const int32_t cy1 = std::min<int32_t>(area.y1 + static_cast<int32_t>(this->ctl_snapshot_->header.h), lh);
  if (cx1 <= cx0 || cy1 <= cy0) {
    this->ctl_shown_.store(nullptr, std::memory_order_release);
    return;
  }
  const int32_t cw = cx1 - cx0;
  const int32_t ch = cy1 - cy0;
  int32_t nx;
  int32_t ny;
  int32_t nw = swapped ? ch : cw;
  int32_t nh = swapped ? cw : ch;
  switch (rotation) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      nx = pw - cy0 - ch;
      ny = cx0;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      nx = pw - cx0 - cw;
      ny = ph - cy0 - ch;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      nx = cy0;
      ny = ph - cx0 - cw;
      break;
    default:
      nx = cx0;
      ny = cy0;
      break;
  }

  // Into the buffer the playback task is not showing.
  uint8_t *shown = this->ctl_shown_.load(std::memory_order_acquire);
  const int slot = shown == this->ctl_bufs_[0] ? 1 : 0;
  const size_t need = sizeof(ControlsBlock) + static_cast<size_t>(nw) * nh * bpp;
  auto busy = [this, slot]() {
    return this->ctl_bufs_[slot] != nullptr &&
           this->ctl_in_use_.load(std::memory_order_acquire) == this->ctl_bufs_[slot];
  };
  for (uint32_t waited = 0; busy() && waited < 20; waited++)
    delay(1);
  if (busy()) {
    this->ctl_dirty_ = true;  // next loop
    return;
  }
  if (this->ctl_buf_size_[slot] < need) {
    heap_caps_free(this->ctl_bufs_[slot]);
    this->ctl_bufs_[slot] = static_cast<uint8_t *>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    this->ctl_buf_size_[slot] = this->ctl_bufs_[slot] != nullptr ? need : 0;
    if (this->ctl_bufs_[slot] == nullptr) {
      ESP_LOGE(TAG, "No PSRAM for the controls (%zu bytes) -- disabled", need);
      this->ctl_failed_ = true;
      this->hide_controls();
      return;
    }
  }
  uint8_t *block = this->ctl_bufs_[slot];
  const ControlsBlock header{static_cast<uint16_t>(nx), static_cast<uint16_t>(ny), static_cast<uint16_t>(nw),
                             static_cast<uint16_t>(nh)};
  memcpy(block, &header, sizeof(header));
  uint8_t *dst = block + sizeof(ControlsBlock);
  const uint8_t *src = this->ctl_snapshot_->data;
  const uint32_t stride = this->ctl_snapshot_->header.stride;
  const int32_t sx = cx0 - area.x1;
  const int32_t sy = cy0 - area.y1;
  for (int32_t r = 0; r < ch; r++) {
    const uint8_t *row = src + static_cast<size_t>(sy + r) * stride + static_cast<size_t>(sx) * bpp;
    for (int32_t c = 0; c < cw; c++) {
      int32_t dx;
      int32_t dy;
      switch (rotation) {
        case display::DISPLAY_ROTATION_90_DEGREES:
          dx = nw - 1 - r;
          dy = c;
          break;
        case display::DISPLAY_ROTATION_180_DEGREES:
          dx = nw - 1 - c;
          dy = nh - 1 - r;
          break;
        case display::DISPLAY_ROTATION_270_DEGREES:
          dx = r;
          dy = nh - 1 - c;
          break;
        default:
          dx = c;
          dy = r;
          break;
      }
      memcpy(dst + (static_cast<size_t>(dy) * nw + dx) * bpp, row + static_cast<size_t>(c) * bpp, bpp);
    }
  }
  this->ctl_shown_.store(block, std::memory_order_release);
  this->ctl_repaint_.store(true, std::memory_order_release);
}

void SimpleVideoPlayer::composite_controls_() {
  uint8_t *block = this->ctl_shown_.load(std::memory_order_acquire);
  if (block == nullptr)
    return;
  this->ctl_in_use_.store(block, std::memory_order_release);
  if (this->ctl_shown_.load(std::memory_order_acquire) == block) {
    ControlsBlock header;
    memcpy(&header, block, sizeof(header));
    const size_t bpp = this->dsi_fb_bpp_;
    const size_t row_bytes = static_cast<size_t>(header.w) * bpp;
    const uint8_t *src = block + sizeof(ControlsBlock);
    for (uint32_t r = 0; r < header.h; r++) {
      memcpy(this->decode_target_ + (static_cast<size_t>(header.y + r) * this->dsi_out_w_ + header.x) * bpp,
             src + r * row_bytes, row_bytes);
    }
  }
  this->ctl_in_use_.store(nullptr, std::memory_order_release);
}

void SimpleVideoPlayer::repaint_paused_() {
  this->composite_controls_();
  const display::ColorBitness bitness =
      this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565;
  this->dsi_->draw_pixels_at(0, 0, this->dsi_out_w_, this->dsi_out_h_, this->decode_target_, display::COLOR_ORDER_RGB,
                             bitness, false, 0, 0, 0);
}

}  // namespace simple_video_player
}  // namespace esphome

#endif  // SVP_CONTROLS
