#include "controls.h"

#ifdef USE_MEDIA_PLAYER_CONTROLS

#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace esphome::media_player {

MediaControls *MediaControls::create() {
  void *mem = RAMAllocator<MediaControls>(RAMAllocator<MediaControls>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) MediaControls();
  return new MediaControls();
}

static const char *button_text(uint8_t button) {
  switch (button) {
    case MediaControls::PREVIOUS:
      return LV_SYMBOL_PREV;
    case MediaControls::PLAY_PAUSE:
      return LV_SYMBOL_PLAY;
    case MediaControls::STOP:
      return LV_SYMBOL_STOP;
    case MediaControls::NEXT:
      return LV_SYMBOL_NEXT;
    case MediaControls::VOLUME_DOWN:
      return LV_SYMBOL_VOLUME_MID;
    case MediaControls::VOLUME_UP:
      return LV_SYMBOL_VOLUME_MAX;
    default:
      return LV_SYMBOL_CLOSE;
  }
}

static lv_obj_t *plain_row(lv_obj_t *parent, lv_style_t *style) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_add_style(row, style, LV_PART_MAIN);
  lv_obj_set_scrollable(row, false);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  return row;
}

void MediaControls::setup() {
  lv_style_init(&this->bar_style_);
  lv_style_set_width(&this->bar_style_, lv_pct(100));
  lv_style_set_height(&this->bar_style_, LV_SIZE_CONTENT);
  lv_style_set_align(&this->bar_style_, LV_ALIGN_BOTTOM_MID);
  lv_style_set_bg_color(&this->bar_style_, lv_color_hex(0x101418));
  lv_style_set_bg_opa(&this->bar_style_, LV_OPA_COVER);
  lv_style_set_pad_hor(&this->bar_style_, 24);
  lv_style_set_pad_ver(&this->bar_style_, 16);
  lv_style_set_pad_row(&this->bar_style_, 14);
  lv_style_set_text_color(&this->bar_style_, lv_color_hex(0xE6EAF0));

  lv_style_init(&this->row_style_);
  lv_style_set_width(&this->row_style_, lv_pct(100));
  lv_style_set_height(&this->row_style_, LV_SIZE_CONTENT);
  lv_style_set_pad_column(&this->row_style_, 12);

  lv_style_init(&this->button_style_);
  lv_style_set_bg_color(&this->button_style_, lv_color_hex(0x232C36));
  lv_style_set_bg_opa(&this->button_style_, LV_OPA_COVER);
  lv_style_set_radius(&this->button_style_, 12);
  lv_style_set_pad_hor(&this->button_style_, 26);
  lv_style_set_pad_ver(&this->button_style_, 14);
  lv_style_set_min_width(&this->button_style_, 88);
  lv_style_set_text_color(&this->button_style_, lv_color_hex(0xFFFFFF));

  lv_style_init(&this->accent_style_);
  lv_style_set_bg_color(&this->accent_style_, lv_color_hex(0x2F81F7));
  lv_style_set_min_width(&this->accent_style_, 120);

  lv_style_init(&this->focus_style_);
  lv_style_set_recolor(&this->focus_style_, lv_color_hex(0xFFFFFF));
  lv_style_set_recolor_opa(&this->focus_style_, LV_OPA_30);

  lv_obj_t *root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root);
  lv_obj_add_style(root, &this->bar_style_, LV_PART_MAIN);
  if (this->user_bar_ != nullptr)
    lv_obj_add_style(root, this->user_bar_, LV_PART_MAIN);
  lv_obj_set_scrollable(root, false);
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_hidden(root, true);
  lv_obj_add_event_cb(
      root, [](lv_event_t *event) { static_cast<MediaControls *>(lv_event_get_user_data(event))->show(); },
      LV_EVENT_PRESSED, this);
  this->bar_ = root;
  if (this->show_title_) {
    lv_obj_t *row = plain_row(root, &this->row_style_);
    this->title_ = lv_label_create(row);
    lv_obj_set_flex_grow(this->title_, 1);
    lv_label_set_long_mode(this->title_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text_static(this->title_, this->title_text_);
  }
  if (this->button_count_ != 0) {
    lv_obj_t *row = plain_row(root, &this->row_style_);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint8_t i = 0; i != this->button_count_; i++) {
      const uint8_t id = this->order_[i];
      if (id >= BUTTON_COUNT || this->button_[id] != nullptr)
        continue;
      lv_obj_t *button = lv_button_create(row);
      lv_obj_remove_style_all(button);
      lv_obj_add_style(button, &this->button_style_, LV_PART_MAIN);
      if (this->user_button_ != nullptr)
        lv_obj_add_style(button, this->user_button_, LV_PART_MAIN);
      if (id == PLAY_PAUSE) {
        lv_obj_add_style(button, &this->accent_style_, LV_PART_MAIN);
        if (this->user_accent_ != nullptr)
          lv_obj_add_style(button, this->user_accent_, LV_PART_MAIN);
      }
      lv_obj_add_style(button, &this->focus_style_, LV_STATE_FOCUS_KEY);
      if (this->user_focus_ != nullptr)
        lv_obj_add_style(button, this->user_focus_, LV_STATE_FOCUS_KEY);
      lv_obj_t *label = lv_label_create(button);
      lv_label_set_text_static(label, button_text(id));
      lv_obj_center(label);
      this->button_[id] = button;
      this->label_[id] = label;
      this->target_[id] = Target{this, id};
      lv_obj_add_event_cb(button, &MediaControls::button_event_, LV_EVENT_CLICKED, &this->target_[id]);
      lv_obj_add_event_cb(
          button, [](lv_event_t *event) { static_cast<MediaControls *>(lv_event_get_user_data(event))->show(); },
          LV_EVENT_FOCUSED, this);
    }
  }
  if (this->player_ != nullptr)
    this->player_->add_on_state_callback([this](MediaPlayerState) { this->update_(); });
#ifdef USE_MEDIA_PLAYER_PICTURES
  if (this->pictures_ != nullptr)
    this->pictures_->add_on_picture_callback([this]() { this->update_(); });
#endif
#ifdef USE_MEDIA_PLAYER_QUEUE
  if (this->queue_ != nullptr) {
    this->queue_->add_on_change_callback([this]() {
      const int index = this->queue_->index();
      if (index >= 0)
        this->set_title(this->queue_->title(static_cast<size_t>(index)));
    });
  }
#endif
}

bool MediaControls::pictures_active_() const {
#ifdef USE_MEDIA_PLAYER_PICTURES
  return this->pictures_ != nullptr && this->pictures_->current_index() >= 0 &&
         (this->player_ == nullptr || this->player_->state != MEDIA_PLAYER_STATE_PLAYING);
#else
  return false;
#endif
}

void MediaControls::update_() {
  if (this->bar_ == nullptr || !this->shown_ || !this->lvgl_runs_())
    return;
  if (this->label_[PLAY_PAUSE] != nullptr) {
    bool running = this->player_ != nullptr && this->player_->state == MEDIA_PLAYER_STATE_PLAYING;
#ifdef USE_MEDIA_PLAYER_PICTURES
    if (this->pictures_active_())
      running = this->pictures_->slideshow_running();
#endif
    lv_label_set_text_static(this->label_[PLAY_PAUSE], running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  }
#ifdef USE_MEDIA_PLAYER_PICTURES
  if (this->title_ != nullptr && this->pictures_active_()) {
    const std::string path = this->pictures_->current_path();
    const size_t slash = path.rfind('/');
    snprintf(this->title_text_, sizeof(this->title_text_), "%s", path.c_str() + (slash == std::string::npos ? 0 : slash + 1));
    lv_label_set_text_static(this->title_, this->title_text_);
  }
#endif
}

void MediaControls::set_title(const std::string &title) {
  snprintf(this->title_text_, sizeof(this->title_text_), "%s", title.c_str());
  if (this->title_ != nullptr && this->lvgl_runs_())
    lv_label_set_text_static(this->title_, this->title_text_);
}

void MediaControls::show() {
  if (this->bar_ == nullptr || !this->lvgl_runs_())
    return;
  if (!this->shown_) {
    this->shown_ = true;
    lv_obj_set_hidden(this->bar_, false);
    lv_group_t *group = lv_group_get_default();
    for (lv_obj_t *button : this->button_) {
      if (button != nullptr && group != nullptr)
        lv_group_add_obj(group, button);
    }
    this->update_();
  }
  if (this->hide_after_ms_ != 0)
    this->set_timeout("hide", this->hide_after_ms_, [this]() { this->hide(); });
}

void MediaControls::hide() {
  this->cancel_timeout("hide");
  if (this->bar_ == nullptr || !this->shown_ || !this->lvgl_runs_())
    return;
  this->shown_ = false;
  for (lv_obj_t *button : this->button_) {
    if (button != nullptr)
      lv_group_remove_obj(button);
  }
  lv_obj_set_hidden(this->bar_, true);
}

void MediaControls::toggle() {
  if (this->shown_) {
    this->hide();
  } else {
    this->show();
  }
}

void MediaControls::button_event_(lv_event_t *event) {
  auto *target = static_cast<Target *>(lv_event_get_user_data(event));
  MediaControls *self = target->self;
  const uint8_t button = target->button;
  self->show();
  self->defer([self, button]() { self->press_(button); });
}

void MediaControls::press_(uint8_t button) {
#ifdef USE_MEDIA_PLAYER_PICTURES
  if (this->pictures_active_()) {
    switch (button) {
      case PREVIOUS:
        this->pictures_->previous();
        return;
      case NEXT:
        this->pictures_->next();
        return;
      case PLAY_PAUSE:
        this->pictures_->slideshow_toggle();
        return;
      case STOP:
        this->pictures_->close();
        return;
      default:
        break;
    }
  }
#endif
#ifdef USE_MEDIA_PLAYER_QUEUE
  if (this->queue_ != nullptr && this->queue_->active()) {
    switch (button) {
      case PREVIOUS:
        this->queue_->previous();
        return;
      case NEXT:
        this->queue_->next();
        return;
      case STOP:
        this->queue_->stop();
        return;
      default:
        break;
    }
  }
#endif
  if (button == EXIT) {
    this->hide();
    this->exit_trigger_.trigger();
    return;
  }
  if (this->player_ == nullptr)
    return;
  static const MediaPlayerCommand COMMANDS[] = {MEDIA_PLAYER_COMMAND_PREVIOUS, MEDIA_PLAYER_COMMAND_TOGGLE,
                                                MEDIA_PLAYER_COMMAND_STOP,     MEDIA_PLAYER_COMMAND_NEXT,
                                                MEDIA_PLAYER_COMMAND_VOLUME_DOWN, MEDIA_PLAYER_COMMAND_VOLUME_UP};
  if (button < sizeof(COMMANDS) / sizeof(COMMANDS[0]))
    this->player_->make_call().set_command(COMMANDS[button]).perform();
}

}

#endif
