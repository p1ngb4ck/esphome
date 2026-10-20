#include "simple_video_player.h"

#ifdef SVP_CONTROLS

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <utility>

#include <esp_cache.h>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "simple_video_player.controls";

static constexpr float SEEK_STEP_S = 10.0f;
static constexpr float VOLUME_STEP = 0.05f;
static constexpr int32_t SEEK_RANGE = 1000;
static const char *const REPEAT_TEXT[3] = {LV_SYMBOL_LOOP " off", LV_SYMBOL_LOOP " 1", LV_SYMBOL_LOOP " all"};

#ifdef SVP_DSI_OUTPUT
static const char *const GLYPHS = "0123456789:";

static bool inside(const lv_area_t &area, int32_t x, int32_t y) {
  return x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2;
}

static int32_t area_w(const lv_area_t &area) { return area.x2 - area.x1 + 1; }

static bool clip(lv_area_t &area, const lv_area_t &to) {
  area.x1 = std::max(area.x1, to.x1);
  area.y1 = std::max(area.y1, to.y1);
  area.x2 = std::min(area.x2, to.x2);
  area.y2 = std::min(area.y2, to.y2);
  return area.x1 <= area.x2 && area.y1 <= area.y2;
}
static int32_t area_h(const lv_area_t &area) { return area.y2 - area.y1 + 1; }
#endif

// "m:ss", or "h:mm:ss" when the file runs an hour or more.
[[maybe_unused]] static size_t format_time(char *out, size_t size, uint32_t seconds, bool hours) {
  const int n = hours ? snprintf(out, size, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, seconds / 3600,
                                 seconds / 60 % 60, seconds % 60)
                      : snprintf(out, size, "%" PRIu32 ":%02" PRIu32, seconds / 60, seconds % 60);
  return n > 0 ? std::min<size_t>(static_cast<size_t>(n), size - 1) : 0;
}

static const char *button_text(uint8_t button) {
  switch (button) {
    case SimpleVideoPlayer::CTL_PREVIOUS:
      return LV_SYMBOL_PREV;
    case SimpleVideoPlayer::CTL_REWIND:
      return "-10";
    case SimpleVideoPlayer::CTL_PLAY_PAUSE:
      return LV_SYMBOL_PAUSE;
    case SimpleVideoPlayer::CTL_FORWARD:
      return "+10";
    case SimpleVideoPlayer::CTL_NEXT:
      return LV_SYMBOL_NEXT;
    case SimpleVideoPlayer::CTL_STOP:
      return LV_SYMBOL_STOP;
    case SimpleVideoPlayer::CTL_VOLUME_DOWN:
      return LV_SYMBOL_MINUS;
    case SimpleVideoPlayer::CTL_VOLUME_UP:
      return LV_SYMBOL_PLUS;
    case SimpleVideoPlayer::CTL_EXIT:
      return LV_SYMBOL_CLOSE;
    default:
      return REPEAT_TEXT[0];
  }
}

void ControlsTouchListener::update(const touchscreen::TouchPoints_t &points) {
  if (points.empty())
    return;
  this->parent_->controls_touch(true, points[0].x, points[0].y);
}

void ControlsTouchListener::release() { this->parent_->controls_touch(false, 0, 0); }

//========================================================================
// Building the bar (setup, LVGL thread)
//========================================================================

void SimpleVideoPlayer::controls_setup_() {
  lv_style_init(&this->ctl_bar_style_);
  lv_style_set_width(&this->ctl_bar_style_, lv_pct(100));
  lv_style_set_height(&this->ctl_bar_style_, LV_SIZE_CONTENT);
  lv_style_set_align(&this->ctl_bar_style_, LV_ALIGN_BOTTOM_MID);
  lv_style_set_bg_color(&this->ctl_bar_style_, lv_color_hex(0x101418));
  lv_style_set_bg_opa(&this->ctl_bar_style_, LV_OPA_COVER);
  lv_style_set_pad_hor(&this->ctl_bar_style_, 24);
  lv_style_set_pad_ver(&this->ctl_bar_style_, 16);
  lv_style_set_pad_row(&this->ctl_bar_style_, 14);
  lv_style_set_text_color(&this->ctl_bar_style_, lv_color_hex(0xE6EAF0));

  lv_style_init(&this->ctl_row_style_);
  lv_style_set_width(&this->ctl_row_style_, lv_pct(100));
  lv_style_set_height(&this->ctl_row_style_, LV_SIZE_CONTENT);
  lv_style_set_pad_column(&this->ctl_row_style_, 12);

  lv_style_init(&this->ctl_button_style_);
  lv_style_set_bg_color(&this->ctl_button_style_, lv_color_hex(0x232C36));
  lv_style_set_bg_opa(&this->ctl_button_style_, LV_OPA_COVER);
  lv_style_set_radius(&this->ctl_button_style_, 12);
  lv_style_set_pad_hor(&this->ctl_button_style_, 26);
  lv_style_set_pad_ver(&this->ctl_button_style_, 14);
  lv_style_set_min_width(&this->ctl_button_style_, 88);
  lv_style_set_text_color(&this->ctl_button_style_, lv_color_hex(0xFFFFFF));

  lv_style_init(&this->ctl_accent_style_);
  lv_style_set_bg_color(&this->ctl_accent_style_, lv_color_hex(0x2F81F7));
  lv_style_set_min_width(&this->ctl_accent_style_, 120);

  // Key focus by colour only (no frame): the element tinted.
  lv_style_init(&this->ctl_focus_style_);
  lv_style_set_recolor(&this->ctl_focus_style_, lv_color_hex(0xFFFFFF));
  lv_style_set_recolor_opa(&this->ctl_focus_style_, LV_OPA_30);

  lv_style_init(&this->ctl_seek_style_);
  lv_style_set_width(&this->ctl_seek_style_, lv_pct(100));
  lv_style_set_height(&this->ctl_seek_style_, 10);
  lv_style_set_radius(&this->ctl_seek_style_, 5);
  lv_style_set_bg_color(&this->ctl_seek_style_, lv_color_hex(0x2A333D));
  lv_style_set_bg_opa(&this->ctl_seek_style_, LV_OPA_COVER);

  lv_style_init(&this->ctl_indicator_style_);
  lv_style_set_radius(&this->ctl_indicator_style_, 5);
  lv_style_set_bg_color(&this->ctl_indicator_style_, lv_color_hex(0x2F81F7));
  lv_style_set_bg_opa(&this->ctl_indicator_style_, LV_OPA_COVER);

#ifdef SVP_DSI_OUTPUT
  if (this->ctl_video_.enabled && this->dsi_ != nullptr && this->build_controls_bar_(this->ctl_video_) &&
      this->ctl_touchscreen_ != nullptr)
    this->ctl_touchscreen_->register_listener(&this->ctl_listener_);
#endif
#ifdef SVP_PICTURES
  if (this->ctl_picture_.enabled && this->pic_canvas_ != nullptr)
    this->build_controls_bar_(this->ctl_picture_);
#endif
#ifdef SVP_AUDIO_CONTROLS
  if (this->ctl_audio_.enabled)
    this->build_controls_bar_(this->ctl_audio_);
#endif
}

static lv_obj_t *plain_row(lv_obj_t *parent, lv_style_t *style) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_add_style(row, style, LV_PART_MAIN);
  lv_obj_set_scrollable(row, false);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  return row;
}

bool SimpleVideoPlayer::build_controls_bar_(ControlsBar &bar) {
  lv_obj_t *root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root);
  lv_obj_add_style(root, &this->ctl_bar_style_, LV_PART_MAIN);
  if (this->ctl_user_bar_style_ != nullptr)
    lv_obj_add_style(root, this->ctl_user_bar_style_, LV_PART_MAIN);
  lv_obj_set_scrollable(root, false);
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_hidden(root, true);
  bar.bar = root;

  if (bar.title || bar.time) {
    lv_obj_t *row = plain_row(root, &this->ctl_row_style_);
    lv_obj_set_flex_align(row, bar.title ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (bar.title) {
      bar.title_label = lv_label_create(row);
      lv_obj_set_flex_grow(bar.title_label, 1);
      lv_label_set_long_mode(bar.title_label, LV_LABEL_LONG_MODE_DOTS);
      lv_label_set_text(bar.title_label, "");
    }
    if (bar.time) {
      bar.position_label = lv_label_create(row);
      lv_obj_set_style_text_align(bar.position_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
      lv_label_set_text(bar.position_label, bar.picture ? "" : "0:00");
      if (!bar.picture) {
        bar.duration_label = lv_label_create(row);
        lv_label_set_text(bar.duration_label, "/ 0:00");
      }
    }
  }
  if (bar.seek_bar) {
    bar.seek = lv_bar_create(root);
    lv_obj_remove_style_all(bar.seek);
    lv_obj_add_style(bar.seek, &this->ctl_seek_style_, LV_PART_MAIN);
    lv_obj_add_style(bar.seek, &this->ctl_indicator_style_, LV_PART_INDICATOR);
    if (this->ctl_user_seek_style_ != nullptr)
      lv_obj_add_style(bar.seek, this->ctl_user_seek_style_, LV_PART_MAIN);
    if (this->ctl_user_indicator_style_ != nullptr)
      lv_obj_add_style(bar.seek, this->ctl_user_indicator_style_, LV_PART_INDICATOR);
    lv_bar_set_range(bar.seek, 0, SEEK_RANGE);
    lv_bar_set_value(bar.seek, 0, LV_ANIM_OFF);
#ifdef SVP_AUDIO_CONTROLS
    if (bar.audio) {
      // A tap or drag on it seeks (on release).
      bar.seek_target = ControlsTarget{this, &bar, CTL_BUTTON_COUNT};
      lv_obj_set_clickable(bar.seek, true);
      lv_obj_add_event_cb(bar.seek, &SimpleVideoPlayer::audio_seek_event_, LV_EVENT_ALL, &bar.seek_target);
    }
#endif
  }
  if (!bar.buttons.empty()) {
    lv_obj_t *row = plain_row(root, &this->ctl_row_style_);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint8_t id : bar.buttons) {
      if (id >= CTL_BUTTON_COUNT || bar.button[id] != nullptr)
        continue;
#ifdef SVP_PICTURES
      if (id == CTL_SLIDE_TIME) {
        if (bar.picture)
          this->build_slide_time_(bar, row);
        continue;
      }
#endif
      lv_obj_t *button = lv_button_create(row);
      lv_obj_remove_style_all(button);
      lv_obj_add_style(button, &this->ctl_button_style_, LV_PART_MAIN);
      if (this->ctl_user_button_style_ != nullptr)
        lv_obj_add_style(button, this->ctl_user_button_style_, LV_PART_MAIN);
      if (id == CTL_PLAY_PAUSE) {
        lv_obj_add_style(button, &this->ctl_accent_style_, LV_PART_MAIN);
        if (this->ctl_user_accent_style_ != nullptr)
          lv_obj_add_style(button, this->ctl_user_accent_style_, LV_PART_MAIN);
      }
      lv_obj_add_style(button, &this->ctl_focus_style_, LV_STATE_FOCUS_KEY);
      if (this->ctl_user_focus_style_ != nullptr)
        lv_obj_add_style(button, this->ctl_user_focus_style_, LV_STATE_FOCUS_KEY);
      lv_obj_t *label = lv_label_create(button);
      lv_label_set_text(label, button_text(id));
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_center(label);
      bar.button[id] = button;
      bar.button_label[id] = label;
      // Over a picture LVGL runs: the buttons act through their events.
      bar.target[id] = ControlsTarget{this, &bar, id};
      if (bar.picture || bar.audio) {
        lv_obj_add_event_cb(button, &SimpleVideoPlayer::controls_button_event_, LV_EVENT_CLICKED, &bar.target[id]);
        // Moving between the buttons with keys keeps the bar shown.
        lv_obj_add_event_cb(
            button,
            [](lv_event_t *event) { static_cast<ControlsBar *>(lv_event_get_user_data(event))->shown_ms = millis(); },
            LV_EVENT_FOCUSED, &bar);
      }
    }
  }
  if (bar.picture || bar.audio) {
    lv_obj_add_event_cb(
        root,
        [](lv_event_t *event) { static_cast<ControlsBar *>(lv_event_get_user_data(event))->shown_ms = millis(); },
        LV_EVENT_PRESSED, &bar);
  }
  return true;
}

#ifdef SVP_PICTURES
// Over a picture: the slideshow's picture duration, a dropdown opening upwards, styled as the buttons.
void SimpleVideoPlayer::build_slide_time_(ControlsBar &bar, lv_obj_t *row) {
  lv_obj_t *dropdown = lv_dropdown_create(row);
  lv_obj_remove_style_all(dropdown);
  lv_obj_add_style(dropdown, &this->ctl_button_style_, LV_PART_MAIN);
  if (this->ctl_user_button_style_ != nullptr)
    lv_obj_add_style(dropdown, this->ctl_user_button_style_, LV_PART_MAIN);
  lv_obj_add_style(dropdown, &this->ctl_focus_style_, LV_STATE_FOCUS_KEY);
  if (this->ctl_user_focus_style_ != nullptr)
    lv_obj_add_style(dropdown, this->ctl_user_focus_style_, LV_STATE_FOCUS_KEY);
  lv_dropdown_set_options(dropdown, slideshow_step_options().c_str());
  lv_dropdown_set_dir(dropdown, LV_DIR_TOP);
  lv_dropdown_set_symbol(dropdown, LV_SYMBOL_UP);
  lv_obj_t *list = lv_dropdown_get_list(dropdown);
  lv_obj_add_style(list, &this->ctl_button_style_, LV_PART_MAIN);
  if (this->ctl_user_button_style_ != nullptr)
    lv_obj_add_style(list, this->ctl_user_button_style_, LV_PART_MAIN);
  const auto selected = static_cast<lv_style_selector_t>(LV_PART_SELECTED) | LV_STATE_CHECKED;
  lv_obj_add_style(list, &this->ctl_accent_style_, selected);
  if (this->ctl_user_accent_style_ != nullptr)
    lv_obj_add_style(list, this->ctl_user_accent_style_, selected);
  bar.button[CTL_SLIDE_TIME] = dropdown;
  bar.target[CTL_SLIDE_TIME] = ControlsTarget{this, &bar, CTL_SLIDE_TIME};
  lv_obj_add_event_cb(
      dropdown,
      [](lv_event_t *event) {
        const auto *target = static_cast<ControlsTarget *>(lv_event_get_user_data(event));
        SimpleVideoPlayer *self = target->player;
        self->picture_controls_touched_();
        const uint32_t index = lv_dropdown_get_selected(lv_event_get_current_target_obj(event));
        if (index >= SLIDESHOW_STEP_COUNT)
          return;
        const uint32_t ms = SLIDESHOW_STEPS_S[index] * 1000u;
        self->set_slideshow_interval(ms);
        self->update_picture_controls_(false);
        self->slideshow_interval_callbacks_.call(ms);
      },
      LV_EVENT_VALUE_CHANGED, &bar.target[CTL_SLIDE_TIME]);
  lv_obj_add_event_cb(
      dropdown,
      [](lv_event_t *event) { static_cast<ControlsBar *>(lv_event_get_user_data(event))->shown_ms = millis(); },
      LV_EVENT_FOCUSED, &bar);
}
#endif

void SimpleVideoPlayer::controls_button_event_(lv_event_t *event) {
  const auto *target = static_cast<ControlsTarget *>(lv_event_get_user_data(event));
#ifdef SVP_AUDIO_CONTROLS
  if (target->bar->audio) {
    SimpleVideoPlayer *self = target->player;
    target->bar->shown_ms = millis();
    switch (target->button) {
      case CTL_PREVIOUS:
        self->audio_previous();
        break;
      case CTL_NEXT:
        self->audio_next();
        break;
      case CTL_PLAY_PAUSE:
        // As the Media page's play button: pause while a track plays, else play.
        if (self->is_audio_session() && self->is_playing()) {
          self->pause();
        } else {
          self->audio_play_or_resume();
        }
        break;
      case CTL_STOP:
        if (self->is_audio_session())
          self->stop();
        break;
      case CTL_EXIT:
        self->hide_audio_controls_();
        target->bar->exit_trigger.trigger();
        return;
      case CTL_VOLUME_DOWN:
      case CTL_VOLUME_UP:
        self->controls_press_(target->button);
        break;
      default:
        break;
    }
    self->update_audio_controls_(false);
    return;
  }
#endif
#ifdef SVP_PICTURES
  if (target->bar->picture) {
    SimpleVideoPlayer *self = target->player;
    self->picture_controls_touched_();
    switch (target->button) {
      case CTL_PREVIOUS:
        self->previous_image();
        break;
      case CTL_NEXT:
        self->next_image();
        break;
      case CTL_PLAY_PAUSE:
        self->toggle_slideshow();
        break;
      case CTL_STOP:
        self->stop_slideshow();
        break;
      case CTL_EXIT:
        self->hide_picture_controls_();
        target->bar->exit_trigger.trigger();
        return;
      case CTL_VOLUME_DOWN:
      case CTL_VOLUME_UP:
        self->controls_press_(target->button);
        break;
      default:
        break;
    }
    self->update_picture_controls_(false);
    return;
  }
#endif
  target->player->controls_press_(target->button);
}

#ifdef SVP_DSI_OUTPUT
size_t SimpleVideoPlayer::controls_bytes_() {
  if (this->ctl_video_.bar == nullptr)
    return 0;
  const lv_color_format_t cf = this->dsi_fb_bpp_ == 3 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565;
  // The size a snapshot of the bar takes (its extra draw area included), measured once.
  lv_draw_buf_t *probe = lv_snapshot_create_draw_buf(this->ctl_video_.bar, cf);
  if (probe == nullptr)
    return 0;
  this->ctl_snap_bytes_ = (probe->data_size + 127) & ~static_cast<size_t>(127);
  // The empty title and duration fields kept beside the bar: one text line across it at most each.
  size_t fields = 0;
  for (lv_obj_t *label : {this->ctl_video_.title_label, this->ctl_video_.duration_label}) {
    if (label != nullptr)
      fields += ((static_cast<size_t>(probe->header.w) *
                      lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)) * this->dsi_fb_bpp_ +
                  15) &
                 ~static_cast<size_t>(15));
  }
  lv_draw_buf_destroy(probe);
  // The snapshot, the bar copied out of it, and the pieces (all inside the bar).
  return 3 * this->ctl_snap_bytes_ + fields + 4096;
}

//========================================================================
// Rendering before a direct session (main loop, LVGL still running)
//========================================================================

uint8_t *SimpleVideoPlayer::ctl_alloc_(size_t bytes) {
  bytes = (bytes + 15) & ~static_cast<size_t>(15);
  if (this->ctl_mem_ == nullptr || this->ctl_mem_used_ + bytes > this->controls_reserve_)
    return nullptr;
  uint8_t *piece = this->ctl_mem_ + this->ctl_mem_used_;
  this->ctl_mem_used_ += bytes;
  return piece;
}

// Where an area of LVGL's screen lies on the panel; the same mapping as LvglComponent's rotated
// flush: 90 puts LVGL's (x, y) at the panel's (W - 1 - y, x).
void SimpleVideoPlayer::ctl_panel_rect_(const lv_area_t &area, int32_t *x, int32_t *y, int32_t *w,
                                        int32_t *h) const {
  const int32_t pw = this->dsi_out_w_;
  const int32_t ph = this->dsi_out_h_;
  const int32_t cw = area_w(area);
  const int32_t ch = area_h(area);
  switch (this->ctl_rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      *x = pw - area.y1 - ch;
      *y = area.x1;
      *w = ch;
      *h = cw;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      *x = pw - area.x1 - cw;
      *y = ph - area.y1 - ch;
      *w = cw;
      *h = ch;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      *x = area.y1;
      *y = ph - area.x1 - cw;
      *w = ch;
      *h = cw;
      break;
    default:
      *x = area.x1;
      *y = area.y1;
      *w = cw;
      *h = ch;
      break;
  }
}

// Copies `part` (LVGL coordinates, clipped to the snapshot and the screen) out of the snapshot,
// turned to the panel's orientation.
bool SimpleVideoPlayer::ctl_take_(ControlsSprite &sprite, const lv_draw_buf_t *snapshot, const lv_area_t &snap_area,
                                  lv_area_t part) {
  lv_display_t *display = lv_obj_get_display(this->ctl_video_.bar);
  const lv_area_t screen{0, 0, lv_display_get_horizontal_resolution(display) - 1,
                         lv_display_get_vertical_resolution(display) - 1};
  if (!clip(part, snap_area) || !clip(part, screen))
    return false;
  int32_t px, py, pw, ph;
  this->ctl_panel_rect_(part, &px, &py, &pw, &ph);
  const size_t bpp = this->dsi_fb_bpp_;
  uint8_t *data = this->ctl_alloc_(static_cast<size_t>(pw) * ph * bpp);
  if (data == nullptr)
    return false;
  const int32_t panel_w = this->dsi_out_w_;
  const int32_t panel_h = this->dsi_out_h_;
  const uint32_t stride = snapshot->header.stride;
  for (int32_t y = part.y1; y <= part.y2; y++) {
    const uint8_t *src = snapshot->data + static_cast<size_t>(y - snap_area.y1) * stride +
                         static_cast<size_t>(part.x1 - snap_area.x1) * bpp;
    // The panel position of (part.x1, y), and how it moves as x grows.
    int32_t X, Y, step;
    switch (this->ctl_rotation_) {
      case display::DISPLAY_ROTATION_90_DEGREES:
        X = panel_w - 1 - y;
        Y = part.x1;
        step = pw;
        break;
      case display::DISPLAY_ROTATION_180_DEGREES:
        X = panel_w - 1 - part.x1;
        Y = panel_h - 1 - y;
        step = -1;
        break;
      case display::DISPLAY_ROTATION_270_DEGREES:
        X = y;
        Y = panel_h - 1 - part.x1;
        step = -pw;
        break;
      default:
        X = part.x1;
        Y = y;
        step = 1;
        break;
    }
    int32_t index = (Y - py) * pw + (X - px);
    const int32_t count = area_w(part);
    if (bpp == 2) {
      const auto *s16 = reinterpret_cast<const uint16_t *>(src);
      auto *d16 = reinterpret_cast<uint16_t *>(data);
      for (int32_t i = 0; i < count; i++, index += step)
        d16[index] = s16[i];
    } else {
      for (int32_t i = 0; i < count; i++, index += step) {
        uint8_t *d = data + static_cast<size_t>(index) * 3;
        d[0] = src[i * 3];
        d[1] = src[i * 3 + 1];
        d[2] = src[i * 3 + 2];
      }
    }
  }
  sprite.area = part;
  sprite.pw = pw;
  sprite.ph = ph;
  sprite.data = data;
  return true;
}

void SimpleVideoPlayer::prerender_controls_() {
  this->ctl_ready_.store(false, std::memory_order_release);
  this->ctl_shown_.store(false, std::memory_order_release);
  ControlsBar &bar = this->ctl_video_;
  if (bar.bar == nullptr || this->ctl_failed_ || this->ctl_mem_ == nullptr || this->is_network_session())
    return;
  if (this->lvgl_component_->is_paused()) {
    // The next file of a playlist, LVGL still paused: the bar as it stands, its title and duration
    // written anew. Not rendered yet (its memory given back meanwhile): this file goes without.
    if (this->ctl_prerendered_) {
      this->ctl_draw_texts_();
      this->ctl_ready_.store(true, std::memory_order_release);
    }
    return;
  }
  this->ctl_prerendered_ = false;
  this->ctl_mem_used_ = 0;
  this->ctl_rotation_ = this->lvgl_component_->get_rotation();
  for (auto &sprite : this->ctl_glyph_)
    sprite = ControlsSprite{};
  this->ctl_play_ = this->ctl_pause_ = this->ctl_progress_ = this->ctl_static_ = ControlsSprite{};
  for (auto &sprite : this->ctl_repeat_sprite_)
    sprite = ControlsSprite{};

  const lv_color_format_t cf = this->dsi_fb_bpp_ == 3 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565;
  lv_draw_buf_t snapshot;
  uint8_t *snap_mem = this->ctl_alloc_(this->ctl_snap_bytes_);
  if (snap_mem == nullptr || lv_draw_buf_init(&snapshot, 1, 1, cf, 0, snap_mem, this->ctl_snap_bytes_) != LV_RESULT_OK)
    return;

  // Title and duration are written per file into the bar (ctl_draw_texts_()): their fields empty
  // here, the bar's layout the same for every file.
  if (bar.title_label != nullptr)
    lv_label_set_text(bar.title_label, " ");
  if (bar.duration_label != nullptr)
    lv_label_set_text(bar.duration_label, " ");
  // Buttons whose text changes keep the width of the widest.
  for (uint8_t id : {CTL_PLAY_PAUSE, CTL_REPEAT}) {
    lv_obj_t *label = bar.button_label[id];
    if (label == nullptr)
      continue;
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    int32_t widest = 0;
    const char *const *texts = id == CTL_REPEAT ? REPEAT_TEXT : nullptr;
    const char *play_pause[2] = {LV_SYMBOL_PLAY, LV_SYMBOL_PAUSE};
    for (int i = 0; i < (id == CTL_REPEAT ? 3 : 2); i++) {
      lv_label_set_text(label, texts != nullptr ? texts[i] : play_pause[i]);
      lv_obj_update_layout(label);
      widest = std::max(widest, lv_obj_get_width(label));
    }
    lv_obj_set_width(label, widest);
  }

  lv_obj_set_hidden(bar.bar, false);
  auto render = [&](lv_area_t *snap_area) -> bool {
    lv_obj_update_layout(bar.bar);
    if (lv_snapshot_take_to_draw_buf(bar.bar, cf, &snapshot) != LV_RESULT_OK)
      return false;
    lv_obj_get_coords(bar.bar, snap_area);
    const int32_t ext = (static_cast<int32_t>(snapshot.header.w) - lv_obj_get_width(bar.bar)) / 2;
    lv_area_increase(snap_area, ext, ext);
    return true;
  };
  auto coords = [](lv_obj_t *obj) {
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return area;
  };
  auto set_play = [&](bool playing) {
    if (bar.button_label[CTL_PLAY_PAUSE] != nullptr)
      lv_label_set_text(bar.button_label[CTL_PLAY_PAUSE], playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  };
  auto set_repeat = [&](int mode) {
    if (bar.button_label[CTL_REPEAT] != nullptr)
      lv_label_set_text(bar.button_label[CTL_REPEAT], REPEAT_TEXT[mode]);
  };

  bool ok = true;
  lv_area_t snap_area;
  const lv_font_t *font = nullptr;
  int32_t letter_space = 0;
  if (bar.position_label != nullptr) {
    font = lv_obj_get_style_text_font(bar.position_label, LV_PART_MAIN);
    letter_space = lv_obj_get_style_text_letter_space(bar.position_label, LV_PART_MAIN);
  }
  for (auto [label, field] : {std::pair{bar.title_label, &this->ctl_title_text_},
                              std::pair{bar.duration_label, &this->ctl_duration_text_}}) {
    *field = ControlsText{};
    if (label == nullptr)
      continue;
    field->font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    field->color = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    field->letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
  }

  // 1: play, repeat off, the seek bar full, every digit.
  set_play(false);
  set_repeat(0);
  if (bar.seek != nullptr)
    lv_bar_set_value(bar.seek, SEEK_RANGE, LV_ANIM_OFF);
  if (bar.position_label != nullptr) {
    lv_obj_set_width(bar.position_label, LV_SIZE_CONTENT);
    lv_label_set_text(bar.position_label, GLYPHS);
  }
  ok = render(&snap_area);
  if (ok && bar.button[CTL_PLAY_PAUSE] != nullptr)
    ok = this->ctl_take_(this->ctl_play_, &snapshot, snap_area, coords(bar.button[CTL_PLAY_PAUSE]));
  if (ok && bar.button[CTL_REPEAT] != nullptr)
    ok = this->ctl_take_(this->ctl_repeat_sprite_[0], &snapshot, snap_area, coords(bar.button[CTL_REPEAT]));
  if (ok && bar.seek != nullptr)
    ok = this->ctl_take_(this->ctl_progress_, &snapshot, snap_area, coords(bar.seek));
  int32_t colon_w = 0;
  if (ok && bar.position_label != nullptr) {
    lv_area_t text;
    lv_obj_get_content_coords(bar.position_label, &text);
    int32_t x = text.x1;
    this->ctl_cell_w_ = 0;
    for (size_t i = 0; ok && i < CTL_GLYPHS; i++) {
      const int32_t w = lv_font_get_glyph_width(font, GLYPHS[i], GLYPHS[i + 1]) + letter_space;
      const lv_area_t glyph{x, text.y1, x + w - 1, text.y2};
      ok = w > 0 && this->ctl_take_(this->ctl_glyph_[i], &snapshot, snap_area, glyph);
      x += w;
      if (GLYPHS[i] == ':')
        colon_w = w;
      else
        this->ctl_cell_w_ = std::max(this->ctl_cell_w_, w);
    }
  }
  // 2: pause, repeat one.
  if (ok && (bar.button[CTL_PLAY_PAUSE] != nullptr || bar.button[CTL_REPEAT] != nullptr)) {
    set_play(true);
    set_repeat(1);
    ok = render(&snap_area);
    if (ok && bar.button[CTL_PLAY_PAUSE] != nullptr)
      ok = this->ctl_take_(this->ctl_pause_, &snapshot, snap_area, coords(bar.button[CTL_PLAY_PAUSE]));
    if (ok && bar.button[CTL_REPEAT] != nullptr)
      ok = this->ctl_take_(this->ctl_repeat_sprite_[1], &snapshot, snap_area, coords(bar.button[CTL_REPEAT]));
  }
  // 3: repeat all.
  if (ok && bar.button[CTL_REPEAT] != nullptr) {
    set_repeat(2);
    ok = render(&snap_area) &&
         this->ctl_take_(this->ctl_repeat_sprite_[2], &snapshot, snap_area, coords(bar.button[CTL_REPEAT]));
  }
  // Last: the bar itself, the seek bar empty and no time; the rest is copied over it.
  if (ok) {
    set_play(true);
    set_repeat(0);
    if (bar.seek != nullptr)
      lv_bar_set_value(bar.seek, 0, LV_ANIM_OFF);
    // Time fields as wide as the longest time ("00:00:00"), for every file alike.
    if (bar.position_label != nullptr) {
      lv_obj_set_width(bar.position_label, 6 * this->ctl_cell_w_ + 2 * colon_w +
                                               lv_obj_get_style_pad_left(bar.position_label, LV_PART_MAIN) +
                                               lv_obj_get_style_pad_right(bar.position_label, LV_PART_MAIN));
      lv_label_set_text(bar.position_label, "");
    }
    if (bar.duration_label != nullptr && this->ctl_duration_text_.font != nullptr) {
      const lv_font_t *dfont = this->ctl_duration_text_.font;
      const int32_t space = this->ctl_duration_text_.letter_space;
      int32_t cell = 0;
      for (char c = '0'; c <= '9'; c++)
        cell = std::max<int32_t>(cell, lv_font_get_glyph_width(dfont, c, 0));
      const int32_t width = lv_font_get_glyph_width(dfont, '/', ' ') + lv_font_get_glyph_width(dfont, ' ', '0') +
                            6 * cell + 2 * lv_font_get_glyph_width(dfont, ':', '0') + 9 * space;
      lv_obj_set_width(bar.duration_label, width + lv_obj_get_style_pad_left(bar.duration_label, LV_PART_MAIN) +
                                               lv_obj_get_style_pad_right(bar.duration_label, LV_PART_MAIN));
    }
    ok = render(&snap_area) && this->ctl_take_(this->ctl_static_, &snapshot, snap_area, snap_area);
    // The empty fields, kept to write each file's text over.
    for (auto [label, field] : {std::pair{bar.title_label, &this->ctl_title_text_},
                                std::pair{bar.duration_label, &this->ctl_duration_text_}}) {
      if (!ok || label == nullptr)
        continue;
      lv_area_t area;
      lv_obj_get_content_coords(label, &area);
      ok = this->ctl_take_(field->background, &snapshot, snap_area, area);
      field->area = field->background.area;  // as far as it lies in the bar
    }
  }
  if (ok) {
    this->ctl_area_ = this->ctl_static_.area;
    if (bar.seek != nullptr)
      this->ctl_seek_area_ = coords(bar.seek);
    if (bar.position_label != nullptr)
      lv_obj_get_content_coords(bar.position_label, &this->ctl_time_area_);
    for (int id = 0; id < CTL_BUTTON_COUNT; id++) {
      if (bar.button[id] != nullptr)
        this->ctl_button_area_[id] = coords(bar.button[id]);
    }
  }
  lv_obj_set_hidden(bar.bar, true);
  // The snapshot's memory is free now: a glyph's coverage goes there.
  ok = ok && lv_draw_buf_init(&this->ctl_glyph_buf_, 1, 1, LV_COLOR_FORMAT_A8, 0, snap_mem, this->ctl_snap_bytes_) ==
                 LV_RESULT_OK;
  if (!ok) {
    ESP_LOGE(TAG, "Could not render the controls (%u bytes of the pool) -- disabled",
             static_cast<unsigned>(this->controls_reserve_));
    this->ctl_failed_ = true;
    return;
  }
  this->ctl_prerendered_ = true;
  this->ctl_draw_texts_();
  this->ctl_ready_.store(true, std::memory_order_release);
}

const char *SimpleVideoPlayer::video_title_() const {
  if (this->avi_parser_ && this->avi_parser_->is_open() && this->avi_parser_->get_title()[0] != '\0')
    return this->avi_parser_->get_title();
  const size_t slash = this->video_path_.rfind('/');
  return this->video_path_.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

// Where LVGL's (x, y) lies in ctl_static_ (the panel's orientation), nullptr outside it.
uint8_t *SimpleVideoPlayer::ctl_static_px_(int32_t x, int32_t y) const {
  const ControlsSprite &bar = this->ctl_static_;
  if (bar.data == nullptr || !inside(bar.area, x, y))
    return nullptr;
  int32_t px, py, pw, ph;
  this->ctl_panel_rect_(bar.area, &px, &py, &pw, &ph);
  int32_t X, Y;
  switch (this->ctl_rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      X = this->dsi_out_w_ - 1 - y;
      Y = x;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      X = this->dsi_out_w_ - 1 - x;
      Y = this->dsi_out_h_ - 1 - y;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      X = y;
      Y = this->dsi_out_h_ - 1 - x;
      break;
    default:
      X = x;
      Y = y;
      break;
  }
  return bar.data + (static_cast<size_t>(Y - py) * bar.pw + (X - px)) * this->dsi_fb_bpp_;
}

void SimpleVideoPlayer::ctl_draw_texts_() {
  // The empty fields back into the bar.
  for (ControlsText *field : {&this->ctl_title_text_, &this->ctl_duration_text_}) {
    const ControlsSprite &bg = field->background;
    if (bg.data == nullptr)
      continue;
    int32_t bx, by, bw, bh, sx, sy, sw, sh;
    this->ctl_panel_rect_(bg.area, &bx, &by, &bw, &bh);
    this->ctl_panel_rect_(this->ctl_static_.area, &sx, &sy, &sw, &sh);
    const size_t bpp = this->dsi_fb_bpp_;
    for (int32_t r = 0; r < bh; r++)
      memcpy(this->ctl_static_.data + (static_cast<size_t>(by - sy + r) * this->ctl_static_.pw + (bx - sx)) * bpp,
             bg.data + static_cast<size_t>(r) * bg.pw * bpp, static_cast<size_t>(bw) * bpp);
  }
  this->ctl_draw_text_(this->ctl_title_text_, this->video_title_(), true);
  char text[24] = "/ ";
  const float duration = this->get_duration();
  format_time(text + 2, sizeof(text) - 2, static_cast<uint32_t>(duration), duration >= 3600.0f);
  this->ctl_draw_text_(this->ctl_duration_text_, text, false);
}

// One UTF-8 character; 0 at the end (or a broken one).
static uint32_t next_letter(const char *&text) {
  const auto *s = reinterpret_cast<const uint8_t *>(text);
  uint32_t letter = s[0];
  int more = 0;
  if (letter == 0)
    return 0;
  if (letter >= 0xF0) {
    letter &= 0x07;
    more = 3;
  } else if (letter >= 0xE0) {
    letter &= 0x0F;
    more = 2;
  } else if (letter >= 0xC0) {
    letter &= 0x1F;
    more = 1;
  } else if (letter >= 0x80) {
    return 0;
  }
  for (int i = 1; i <= more; i++) {
    if ((s[i] & 0xC0) != 0x80)
      return 0;
    letter = (letter << 6) | (s[i] & 0x3F);
  }
  text += more + 1;
  return letter;
}

// As LVGL's label draws one line (lv_draw_label: a letter at x + ofs_x, line top + (line_height -
// base_line) - box_h - ofs_y, then on by adv_w + letter_space); too long: cut with "..." as
// LV_LABEL_LONG_MODE_DOTS does.
void SimpleVideoPlayer::ctl_draw_text_(ControlsText &field, const char *text, bool dots) {
  const lv_font_t *font = field.font;
  if (font == nullptr || field.background.data == nullptr || text == nullptr)
    return;
  auto advance = [&](uint32_t letter, uint32_t next) -> int32_t {
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(font, &g, letter, next) ? g.adv_w + field.letter_space : 0;
  };
  const int32_t room = area_w(field.area);
  int32_t width = 0;
  for (const char *p = text; *p != '\0';) {
    const uint32_t letter = next_letter(p);
    if (letter == 0)
      break;
    const char *q = p;
    width += advance(letter, next_letter(q));
  }
  const char *stop = nullptr;  // where the dots go instead
  if (dots && width > room) {
    const int32_t dots_w = 3 * advance('.', '.');
    int32_t used = 0;
    stop = text;
    for (const char *p = text; *p != '\0';) {
      const uint32_t letter = next_letter(p);
      if (letter == 0)
        break;
      const char *q = p;
      const int32_t w = advance(letter, next_letter(q));
      if (used + w + dots_w > room)
        break;
      used += w;
      stop = p;
    }
  }
  const lv_color_t c = field.color;
  const bool rgb888 = this->dsi_fb_bpp_ == 3;
  const int32_t top = field.area.y1 + (font->line_height - font->base_line);
  int32_t x = field.area.x1;
  auto draw = [&](uint32_t letter, uint32_t next) {
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, letter, next))
      return;
    const int32_t x1 = x + g.ofs_x;
    const int32_t y1 = top - g.box_h - g.ofs_y;
    x += g.adv_w + field.letter_space;
    if (g.box_w == 0 || g.box_h == 0)
      return;
    const void *got = lv_font_get_glyph_bitmap(&g, &this->ctl_glyph_buf_);
    if (got == nullptr)
      return;
    // A8 rows as the font wrote them (lv_font_get_bitmap_fmt_txt(), font::Font::get_glyph_bitmap()).
    const uint8_t *cover = this->ctl_glyph_buf_.data;
    const uint32_t stride = lv_draw_buf_width_to_stride(g.box_w, LV_COLOR_FORMAT_A8);
    for (int32_t gy = 0; gy < g.box_h; gy++) {
      for (int32_t gx = 0; gx < g.box_w; gx++) {
        const uint32_t a = cover[gy * stride + gx];
        if (a == 0 || !inside(field.area, x1 + gx, y1 + gy))
          continue;
        uint8_t *px = this->ctl_static_px_(x1 + gx, y1 + gy);
        if (px == nullptr)
          continue;
        const uint32_t na = 255 - a;
        if (rgb888) {  // LVGL's RGB888: blue, green, red
          px[0] = static_cast<uint8_t>((c.blue * a + px[0] * na + 127) / 255);
          px[1] = static_cast<uint8_t>((c.green * a + px[1] * na + 127) / 255);
          px[2] = static_cast<uint8_t>((c.red * a + px[2] * na + 127) / 255);
        } else {  // RGB565 as lv_color_to_u16()
          uint16_t v;
          memcpy(&v, px, 2);
          const uint32_t r = ((c.red >> 3) * a + ((v >> 11) & 0x1F) * na + 127) / 255;
          const uint32_t gr = ((c.green >> 2) * a + ((v >> 5) & 0x3F) * na + 127) / 255;
          const uint32_t b = ((c.blue >> 3) * a + (v & 0x1F) * na + 127) / 255;
          v = static_cast<uint16_t>((r << 11) | (gr << 5) | b);
          memcpy(px, &v, 2);
        }
      }
    }
  };
  for (const char *p = text; *p != '\0' && p != stop;) {
    const uint32_t letter = next_letter(p);
    if (letter == 0)
      break;
    const char *q = p;
    draw(letter, stop != nullptr && p == stop ? '.' : next_letter(q));
  }
  if (stop != nullptr) {
    for (int i = 0; i < 3; i++)
      draw('.', i < 2 ? '.' : 0);
  }
}

#else
size_t SimpleVideoPlayer::controls_bytes_() { return 0; }
void SimpleVideoPlayer::prerender_controls_() {}
#endif

//========================================================================
// Showing, touches (main loop)
//========================================================================

bool SimpleVideoPlayer::controls_shown() const {
#ifdef SVP_AUDIO_CONTROLS
  if (this->ctl_audio_.visible && this->lvgl_component_ != nullptr && !this->lvgl_component_->is_paused())
    return true;
#endif
#ifdef SVP_PICTURES
  if (this->ctl_picture_.bar != nullptr && !lv_obj_is_hidden(this->ctl_picture_.bar))
    return true;
#endif
  return this->ctl_shown_.load(std::memory_order_acquire);
}

#ifdef SVP_DSI_OUTPUT
bool SimpleVideoPlayer::controls_active_() const {
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  return this->ctl_ready_.load(std::memory_order_acquire) && !this->ctl_failed_ && this->dsi_lvgl_paused_ &&
         (state == PlayerState::PLAYING || state == PlayerState::PAUSED) && !this->is_network_session();
}

void SimpleVideoPlayer::show_controls() {
#ifdef SVP_AUDIO_CONTROLS
  // Over the cover art while LVGL shows it; a video over everything has its own.
  if (this->audio_fullscreen_ && this->lvgl_component_ != nullptr && !this->lvgl_component_->is_paused()) {
    this->show_audio_controls_();
    return;
  }
#endif
#ifdef SVP_PICTURES
  if (this->fullscreen_) {
    this->show_picture_controls_();
    return;
  }
#endif
  if (!this->controls_active_())
    return;
  this->ctl_last_input_ms_ = millis();
  this->ctl_shown_.store(true, std::memory_order_release);
  this->ctl_repaint_.store(true, std::memory_order_release);
  this->playback_wait_.wake();
}

void SimpleVideoPlayer::hide_controls() {
#ifdef SVP_AUDIO_CONTROLS
  this->hide_audio_controls_();
#endif
  this->ctl_dragging_.store(false, std::memory_order_release);
  if (this->ctl_shown_.exchange(false, std::memory_order_acq_rel)) {
    this->ctl_repaint_.store(true, std::memory_order_release);
    this->playback_wait_.wake();
  }
}

float SimpleVideoPlayer::controls_fraction_(int32_t x) const {
  const int32_t width = area_w(this->ctl_seek_area_);
  if (width <= 1)
    return 0.0f;
  const float fraction = static_cast<float>(x - this->ctl_seek_area_.x1) / static_cast<float>(width - 1);
  return fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
}

void SimpleVideoPlayer::controls_touch(bool pressed, int32_t x, int32_t y) {
#ifdef SVP_DSI_OUTPUT
  // The camera full screen: a tap (press and release) tells its owner, who ends it
  if (this->cam_direct_ready_.load(std::memory_order_acquire)) {
    if (pressed) {
      this->cam_direct_touch_ = true;
    } else if (this->cam_direct_touch_) {
      this->cam_direct_touch_ = false;
      this->cam_direct_tap_callbacks_.call();
    }
    return;
  }
#endif
  if (!this->controls_active_()) {
    this->ctl_pressed_ = false;
    this->ctl_dragging_.store(false, std::memory_order_release);
    return;
  }
  this->ctl_last_input_ms_ = millis();
  const bool shown = this->ctl_shown_.load(std::memory_order_acquire);
  if (pressed) {
    // Touches come in panel coordinates; the controls are laid out in LVGL's.
    this->lvgl_component_->rotate_coordinates(x, y);
    if (!this->ctl_pressed_) {
      this->ctl_pressed_ = true;
      this->ctl_dragging_.store(shown && this->ctl_video_.seek != nullptr && this->can_seek() &&
                                    inside(this->ctl_seek_area_, x, y),
                                std::memory_order_release);
    }
    this->ctl_x_ = x;
    this->ctl_y_ = y;
    if (this->ctl_dragging_.load(std::memory_order_relaxed)) {
      this->ctl_drag_permille_.store(static_cast<uint16_t>(this->controls_fraction_(x) * SEEK_RANGE),
                                     std::memory_order_release);
      this->ctl_repaint_.store(true, std::memory_order_release);
      this->playback_wait_.wake();
    }
    return;
  }
  if (!this->ctl_pressed_)
    return;
  this->ctl_pressed_ = false;
  if (this->ctl_dragging_.load(std::memory_order_relaxed)) {
    const float fraction = this->ctl_drag_permille_.load(std::memory_order_relaxed) / static_cast<float>(SEEK_RANGE);
    this->seek(fraction * this->get_duration());
    this->ctl_dragging_.store(false, std::memory_order_release);
    this->ctl_repaint_.store(true, std::memory_order_release);
    this->playback_wait_.wake();
    return;
  }
  if (!shown) {
    this->show_controls();
    return;
  }
  if (!inside(this->ctl_area_, this->ctl_x_, this->ctl_y_)) {
    // Beside the controls: hidden again (they stay while paused).
    if (this->is_playing())
      this->hide_controls();
    return;
  }
  for (uint8_t id = 0; id < CTL_BUTTON_COUNT; id++) {
    if (this->ctl_video_.button[id] != nullptr && inside(this->ctl_button_area_[id], this->ctl_x_, this->ctl_y_)) {
      this->controls_press_(id);
      break;
    }
  }
  this->ctl_repaint_.store(true, std::memory_order_release);
  this->playback_wait_.wake();
}

#else
bool SimpleVideoPlayer::controls_active_() const { return false; }
void SimpleVideoPlayer::show_controls() {
#ifdef SVP_AUDIO_CONTROLS
  // Over the cover art while LVGL shows it; a video over everything has its own.
  if (this->audio_fullscreen_ && this->lvgl_component_ != nullptr && !this->lvgl_component_->is_paused()) {
    this->show_audio_controls_();
    return;
  }
#endif
#ifdef SVP_PICTURES
  if (this->fullscreen_)
    this->show_picture_controls_();
#endif
}
void SimpleVideoPlayer::hide_controls() {
#ifdef SVP_AUDIO_CONTROLS
  this->hide_audio_controls_();
#endif
}
void SimpleVideoPlayer::controls_touch(bool pressed, int32_t x, int32_t y) {}
#endif

void SimpleVideoPlayer::controls_press_(uint8_t button) {
  switch (button) {
    case CTL_PREVIOUS:
      this->previous();
      break;
    case CTL_REWIND:
      if (this->can_seek())
        this->seek_relative(-SEEK_STEP_S);
      break;
    case CTL_PLAY_PAUSE:
      this->toggle_pause();
      break;
    case CTL_FORWARD:
      if (this->can_seek())
        this->seek_relative(SEEK_STEP_S);
      break;
    case CTL_NEXT:
      this->next();
      break;
    case CTL_STOP:
    case CTL_EXIT:
      this->stop();
      break;
    case CTL_REPEAT:
      this->set_repeat(this->repeat_ == RepeatMode::OFF    ? RepeatMode::FILE
                       : this->repeat_ == RepeatMode::FILE ? RepeatMode::PLAYLIST
                                                           : RepeatMode::OFF);
      break;
    case CTL_VOLUME_DOWN:
    case CTL_VOLUME_UP:
      this->set_volume(std::min(std::max(this->get_volume() + (button == CTL_VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP),
                                         0.0f),
                                1.0f));
      break;
    default:
      break;
  }
}

void SimpleVideoPlayer::controls_loop_() {
#ifdef SVP_PICTURES
  this->picture_controls_loop_();
#endif
#ifdef SVP_AUDIO_CONTROLS
  this->audio_controls_loop_();
#endif
  if (this->ctl_video_.bar == nullptr)
    return;
  if (!this->controls_active_()) {
    if (this->ctl_shown_.load(std::memory_order_relaxed))
      this->hide_controls();
    this->ctl_pressed_ = false;
    return;
  }
  if (this->ctl_shown_.load(std::memory_order_relaxed) &&
      this->state_.load(std::memory_order_acquire) == PlayerState::PLAYING && !this->ctl_pressed_ &&
      millis() - this->ctl_last_input_ms_ > this->ctl_hide_after_ms_)
    this->hide_controls();
}

#ifdef SVP_PICTURES
//========================================================================
// Over a picture in full screen (main loop, LVGL shows them)
//========================================================================

void SimpleVideoPlayer::show_picture_controls_() {
  ControlsBar &bar = this->ctl_picture_;
  if (bar.bar == nullptr || !this->fullscreen_ || this->pictures_held_())
    return;
  this->update_picture_controls_(true);
  lv_obj_move_to_index(bar.bar, -1);
  lv_obj_set_hidden(bar.bar, false);
  bar.shown_ms = millis();
  // Keys go to the bar: its play/pause button, else its first one.
  lv_obj_t *focus = bar.button[CTL_PLAY_PAUSE];
  for (uint8_t id = 0; focus == nullptr && id < CTL_BUTTON_COUNT; id++)
    focus = bar.button[id];
  if (focus != nullptr)
    lv_group_focus_obj(focus);
}

void SimpleVideoPlayer::hide_picture_controls_() {
  lv_obj_t *slide_time = this->ctl_picture_.button[CTL_SLIDE_TIME];
  if (slide_time != nullptr && lv_dropdown_is_open(slide_time))
    lv_dropdown_close(slide_time);
  if (this->ctl_picture_.bar != nullptr)
    lv_obj_set_hidden(this->ctl_picture_.bar, true);
}

void SimpleVideoPlayer::picture_controls_tap_() {
  if (this->ctl_picture_.bar == nullptr)
    return;
  if (lv_obj_is_hidden(this->ctl_picture_.bar)) {
    this->show_picture_controls_();
  } else {
    this->hide_picture_controls_();
  }
}

void SimpleVideoPlayer::picture_controls_touched_() { this->ctl_picture_.shown_ms = millis(); }

// The texts only when they changed: what LVGL redraws.
void SimpleVideoPlayer::update_picture_controls_(bool force) {
  ControlsBar &bar = this->ctl_picture_;
  const int image = this->current_image_;
  const int count = static_cast<int>(this->images_.size());
  const int playing = this->slideshow_mode_ == SlideshowMode::PLAYING ? 1 : 0;
  if (force || image != bar.shown_image || count != bar.shown_count) {
    if (bar.title_label != nullptr)
      lv_label_set_text(bar.title_label,
                        image >= 0 && image < count ? this->images_[image].filename.c_str() : "");
    if (bar.position_label != nullptr) {
      char text[32] = "";  // two ints and " / ": up to 25 with the end
      if (image >= 0 && image < count)
        snprintf(text, sizeof(text), "%d / %d", image + 1, count);
      lv_label_set_text(bar.position_label, text);
    }
    bar.shown_image = image;
    bar.shown_count = count;
  }
  if ((force || playing != bar.shown_playing) && bar.button_label[CTL_PLAY_PAUSE] != nullptr) {
    lv_label_set_text(bar.button_label[CTL_PLAY_PAUSE], playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    bar.shown_playing = playing;
  }
  lv_obj_t *slide_time = bar.button[CTL_SLIDE_TIME];
  if (slide_time != nullptr && (force || bar.shown_slide != this->slideshow_interval_ms_)) {
    snprintf(bar.slide_text, sizeof(bar.slide_text), "%u s",
             static_cast<unsigned>((this->slideshow_interval_ms_ + 500) / 1000));
    lv_dropdown_set_text(slide_time, bar.slide_text);
    lv_dropdown_set_selected(slide_time, slideshow_step_index(this->slideshow_interval_ms_));
    bar.shown_slide = this->slideshow_interval_ms_;
  }
}

void SimpleVideoPlayer::picture_controls_loop_() {
  ControlsBar &bar = this->ctl_picture_;
  if (bar.bar == nullptr || lv_obj_is_hidden(bar.bar))
    return;
  // Its slide_time open: the bar stays.
  if (bar.button[CTL_SLIDE_TIME] != nullptr && lv_dropdown_is_open(bar.button[CTL_SLIDE_TIME]))
    bar.shown_ms = millis();
  if (!this->fullscreen_ || this->pictures_held_() || millis() - bar.shown_ms > this->ctl_hide_after_ms_) {
    this->hide_picture_controls_();
    return;
  }
  this->update_picture_controls_(false);
}
#endif

#ifdef SVP_AUDIO_CONTROLS
//========================================================================
// Over the audio queue's cover art in full screen (main loop, LVGL shows them)
//========================================================================

static bool lvgl_paused(lvgl::LvglComponent *lvgl) { return lvgl == nullptr || lvgl->is_paused(); }

void SimpleVideoPlayer::set_audio_fullscreen(bool fullscreen) {
  this->audio_fullscreen_ = fullscreen;
  if (!fullscreen)
    this->hide_audio_controls_();
}

void SimpleVideoPlayer::show_audio_controls_() {
  ControlsBar &bar = this->ctl_audio_;
  if (bar.bar == nullptr || !this->audio_fullscreen_ || lvgl_paused(this->lvgl_component_))
    return;
  this->update_audio_controls_(true);
  lv_obj_move_to_index(bar.bar, -1);
  lv_obj_set_hidden(bar.bar, false);
  bar.visible = true;
  bar.shown_ms = millis();
  // Keys go to the bar: its play/pause button, else its first one.
  lv_obj_t *focus = bar.button[CTL_PLAY_PAUSE];
  for (uint8_t id = 0; focus == nullptr && id < CTL_BUTTON_COUNT; id++)
    focus = bar.button[id];
  if (focus != nullptr)
    lv_group_focus_obj(focus);
}

void SimpleVideoPlayer::hide_audio_controls_() {
  ControlsBar &bar = this->ctl_audio_;
  if (!bar.visible || lvgl_paused(this->lvgl_component_))
    return;  // paused: hidden once LVGL runs again (audio_controls_loop_())
  lv_obj_set_hidden(bar.bar, true);
  bar.visible = false;
  bar.dragging = false;
}

// The texts only when they changed: what LVGL redraws.
void SimpleVideoPlayer::update_audio_controls_(bool force) {
  ControlsBar &bar = this->ctl_audio_;
  const int track = this->audio_queue_index();
  if (force || track != bar.shown_image || this->audio_queue_gen_ != bar.shown_gen) {
    if (bar.title_label != nullptr)
      lv_label_set_text(bar.title_label, track >= 0 ? this->audio_queue_title(track).c_str() : "");
    bar.shown_image = track;
    bar.shown_gen = this->audio_queue_gen_;
  }
  const bool session = this->is_audio_session();
  const int playing = session && this->is_playing() ? 1 : 0;
  if ((force || playing != bar.shown_playing) && bar.button_label[CTL_PLAY_PAUSE] != nullptr) {
    lv_label_set_text(bar.button_label[CTL_PLAY_PAUSE], playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    bar.shown_playing = playing;
  }
  // Time and seek bar only while the track's length is known.
  const float duration = session ? this->get_duration() : 0.0f;
  const int total = static_cast<int>(duration);
  if (force || total != bar.shown_count) {
    const bool known = total > 0;
    for (lv_obj_t *part : {bar.position_label, bar.duration_label, bar.seek}) {
      if (part != nullptr && lv_obj_is_hidden(part) == known)
        lv_obj_set_hidden(part, !known);
    }
    if (known && bar.duration_label != nullptr) {
      char text[24] = "/ ";
      format_time(text + 2, sizeof(text) - 2, static_cast<uint32_t>(total), total >= 3600);
      lv_label_set_text(bar.duration_label, text);
    }
    bar.shown_count = total;
    bar.shown_position = -1;
  }
  if (total <= 0)
    return;
  const float fraction = bar.dragging ? bar.drag_permille / static_cast<float>(SEEK_RANGE)
                                      : std::min(std::max(this->get_position() / duration, 0.0f), 1.0f);
  const int position = static_cast<int>(fraction * duration);
  if (force || position != bar.shown_position) {
    if (bar.position_label != nullptr) {
      char text[16];
      format_time(text, sizeof(text), static_cast<uint32_t>(position), total >= 3600);
      lv_label_set_text(bar.position_label, text);
    }
    if (bar.seek != nullptr)
      lv_bar_set_value(bar.seek, static_cast<int32_t>(fraction * SEEK_RANGE), LV_ANIM_OFF);
    bar.shown_position = position;
  }
}

void SimpleVideoPlayer::audio_controls_loop_() {
  ControlsBar &bar = this->ctl_audio_;
  if (!bar.visible || lvgl_paused(this->lvgl_component_))
    return;
  // As over a video: hidden after hide_after while it plays, kept while paused or stopped.
  const bool playing = this->is_audio_session() && this->is_playing();
  if (!this->audio_fullscreen_ || (playing && !bar.dragging && millis() - bar.shown_ms > this->ctl_hide_after_ms_)) {
    this->hide_audio_controls_();
    return;
  }
  this->update_audio_controls_(false);
}

// The seek bar: where it is pressed, shown while dragged, sought on release.
void SimpleVideoPlayer::audio_seek_event_(lv_event_t *event) {
  const auto *target = static_cast<ControlsTarget *>(lv_event_get_user_data(event));
  SimpleVideoPlayer *self = target->player;
  ControlsBar &bar = *target->bar;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
      code != LV_EVENT_PRESS_LOST)
    return;
  bar.shown_ms = millis();
  const float duration = self->get_duration();
  if (!self->can_seek() || duration <= 0.0f) {
    bar.dragging = false;
    return;
  }
  if (code == LV_EVENT_PRESS_LOST) {
    bar.dragging = false;
    return;
  }
  lv_indev_t *indev = lv_indev_active();
  if (indev == nullptr || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
    return;
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_area_t area;
  lv_obj_get_coords(bar.seek, &area);
  const int32_t width = area.x2 - area.x1;
  const float fraction =
      width > 0 ? std::min(std::max(static_cast<float>(point.x - area.x1) / static_cast<float>(width), 0.0f), 1.0f)
                : 0.0f;
  bar.drag_permille = static_cast<uint16_t>(fraction * SEEK_RANGE);
  bar.dragging = code != LV_EVENT_RELEASED;
  if (code == LV_EVENT_RELEASED)
    self->seek(fraction * duration);
  self->update_audio_controls_(false);
}
#endif

//========================================================================
// Into the frame (playback task): copies only, no LVGL
//========================================================================

#ifdef SVP_DSI_OUTPUT
// The sprite with its top left at LVGL's (x, y); only its first `width` columns when given.
void SimpleVideoPlayer::ctl_blit_(const ControlsSprite &sprite, int32_t x, int32_t y, int32_t width) {
  if (sprite.data == nullptr)
    return;
  const int32_t sw = area_w(sprite.area);
  const int32_t sh = area_h(sprite.area);
  const lv_area_t whole{x, y, x + sw - 1, y + sh - 1};
  const lv_area_t part{x, y, x + (width < 0 ? sw : std::min(width, sw)) - 1, y + sh - 1};
  if (part.x2 < part.x1)
    return;
  int32_t wx, wy, ww, wh, px, py, pw, ph;
  this->ctl_panel_rect_(whole, &wx, &wy, &ww, &wh);
  this->ctl_panel_rect_(part, &px, &py, &pw, &ph);
  const size_t bpp = this->dsi_fb_bpp_;
  const size_t row = static_cast<size_t>(pw) * bpp;
  const uint8_t *src = sprite.data + (static_cast<size_t>(py - wy) * sprite.pw + (px - wx)) * bpp;
  uint8_t *const first = this->direct_frame_ + (static_cast<size_t>(py) * this->dsi_out_w_ + px) * bpp;
  uint8_t *dst = first;
  const size_t dst_stride = static_cast<size_t>(this->dsi_out_w_) * bpp;
  const size_t src_stride = static_cast<size_t>(sprite.pw) * bpp;
  for (int32_t r = 0; r < ph; r++, src += src_stride, dst += dst_stride)
    memcpy(dst, src, row);
  // One of the display's buffers is scanned out from memory: the rows written back from the cache.
  if (this->direct_frame_ != this->decode_target_ && ph > 0)
    esp_cache_msync(first, static_cast<size_t>(ph - 1) * dst_stride + row,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void SimpleVideoPlayer::composite_controls_() {
  if (!this->ctl_ready_.load(std::memory_order_acquire) || !this->ctl_shown_.load(std::memory_order_acquire) ||
      this->direct_frame_ == nullptr)
    return;
  this->ctl_blit_(this->ctl_static_, this->ctl_static_.area.x1, this->ctl_static_.area.y1);
  const ControlsSprite &play_pause =
      this->state_.load(std::memory_order_relaxed) == PlayerState::PLAYING ? this->ctl_pause_ : this->ctl_play_;
  this->ctl_blit_(play_pause, play_pause.area.x1, play_pause.area.y1);
  const ControlsSprite &repeat = this->ctl_repeat_sprite_[static_cast<int>(this->repeat_) % 3];
  this->ctl_blit_(repeat, repeat.area.x1, repeat.area.y1);

  const float duration = this->get_duration();
  const bool dragging = this->ctl_dragging_.load(std::memory_order_acquire);
  float fraction = dragging ? this->ctl_drag_permille_.load(std::memory_order_relaxed) / static_cast<float>(SEEK_RANGE)
                            : (duration > 0.0f ? this->get_position() / duration : 0.0f);
  fraction = std::min(std::max(fraction, 0.0f), 1.0f);
  if (this->ctl_progress_.data != nullptr) {
    const auto fill = static_cast<int32_t>(fraction * area_w(this->ctl_progress_.area) + 0.5f);
    this->ctl_blit_(this->ctl_progress_, this->ctl_progress_.area.x1, this->ctl_progress_.area.y1, fill);
  }
  if (this->ctl_glyph_[0].data != nullptr) {
    char text[16];
    const float seconds = dragging ? fraction * duration : this->get_position();
    const size_t len = format_time(text, sizeof(text), static_cast<uint32_t>(seconds), duration >= 3600.0f);
    // Right-aligned; digits in cells as wide as the widest, so the time does not jitter.
    int32_t x = this->ctl_time_area_.x2 + 1;
    for (size_t i = len; i-- > 0;) {
      const size_t g = text[i] == ':' ? 10 : static_cast<size_t>(text[i] - '0');
      if (g >= CTL_GLYPHS)
        continue;
      const ControlsSprite &glyph = this->ctl_glyph_[g];
      const int32_t gw = area_w(glyph.area);
      const int32_t cell = text[i] == ':' ? gw : this->ctl_cell_w_;
      x -= cell;
      if (x < this->ctl_time_area_.x1)
        break;
      this->ctl_blit_(glyph, x + (cell - gw) / 2, glyph.area.y1);
    }
  }
}

void SimpleVideoPlayer::repaint_paused_() {
  if (this->direct_frame_ == nullptr)
    return;
  this->composite_controls_();
  this->direct_show_(this->direct_frame_, 0, 0, this->dsi_out_w_, this->dsi_out_h_, 0);
}

#else
void SimpleVideoPlayer::composite_controls_() {}
void SimpleVideoPlayer::repaint_paused_() {}
#endif

}  // namespace simple_video_player
}  // namespace esphome

#endif  // SVP_CONTROLS
