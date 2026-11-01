#pragma once

#include "esphome/core/defines.h"
#ifdef USE_MEDIA_PLAYER_CONTROLS

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "media_player.h"
#ifdef USE_MEDIA_PLAYER_PICTURES
#include "pictures.h"
#endif
#ifdef USE_MEDIA_PLAYER_QUEUE
#include "queue.h"
#endif

#include <string>

namespace esphome::media_player {

class MediaControls : public Component {
 public:
  enum Button : uint8_t { PREVIOUS, PLAY_PAUSE, STOP, NEXT, VOLUME_DOWN, VOLUME_UP, EXIT, BUTTON_COUNT };

  static MediaControls *create();

  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_player(MediaPlayer *player) { this->player_ = player; }
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
#ifdef USE_MEDIA_PLAYER_PICTURES
  void set_pictures(MediaPictures *pictures) { this->pictures_ = pictures; }
#endif
#ifdef USE_MEDIA_PLAYER_QUEUE
  void set_queue(MediaQueue *queue) { this->queue_ = queue; }
#endif
  void set_hide_after(uint32_t ms) { this->hide_after_ms_ = ms; }
  void add_button(uint8_t button) {
    if (this->button_count_ < BUTTON_COUNT)
      this->order_[this->button_count_++] = button;
  }
  void set_show_title(bool title) { this->show_title_ = title; }
  void set_bar_style(lv_style_t *style) { this->user_bar_ = style; }
  void set_button_style(lv_style_t *style) { this->user_button_ = style; }
  void set_accent_style(lv_style_t *style) { this->user_accent_ = style; }
  void set_focus_style(lv_style_t *style) { this->user_focus_ = style; }
  Trigger<> *get_exit_trigger() { return &this->exit_trigger_; }

  void show();
  void hide();
  void toggle();
  bool is_shown() const { return this->shown_; }
  void set_title(const std::string &title);

 protected:
  static void button_event_(lv_event_t *event);
  void press_(uint8_t button);
  void update_();
  bool lvgl_runs_() const { return this->lvgl_ != nullptr && !this->lvgl_->is_paused(); }
  bool pictures_active_() const;

  MediaPlayer *player_{nullptr};
  lvgl::LvglComponent *lvgl_{nullptr};
#ifdef USE_MEDIA_PLAYER_PICTURES
  MediaPictures *pictures_{nullptr};
#endif
#ifdef USE_MEDIA_PLAYER_QUEUE
  MediaQueue *queue_{nullptr};
#endif
  uint32_t hide_after_ms_{5000};
  uint8_t order_[BUTTON_COUNT]{};
  uint8_t button_count_{0};
  bool show_title_{true};
  lv_style_t *user_bar_{nullptr};
  lv_style_t *user_button_{nullptr};
  lv_style_t *user_accent_{nullptr};
  lv_style_t *user_focus_{nullptr};
  lv_style_t bar_style_{};
  lv_style_t row_style_{};
  lv_style_t button_style_{};
  lv_style_t accent_style_{};
  lv_style_t focus_style_{};
  lv_obj_t *bar_{nullptr};
  lv_obj_t *title_{nullptr};
  lv_obj_t *button_[BUTTON_COUNT]{};
  lv_obj_t *label_[BUTTON_COUNT]{};
  struct Target {
    MediaControls *self;
    uint8_t button;
  };
  Target target_[BUTTON_COUNT]{};
  char title_text_[96]{};
  bool shown_{false};
  Trigger<> exit_trigger_;
};

template<typename... Ts> class ControlsAction : public Action<Ts...>, public Parented<MediaControls> {
 public:
  void set_mode(uint8_t mode) { this->mode_ = mode; }
  void play(const Ts &...x) override {
    if (this->mode_ == 0) {
      this->parent_->show();
    } else if (this->mode_ == 1) {
      this->parent_->hide();
    } else {
      this->parent_->toggle();
    }
  }

 protected:
  uint8_t mode_{0};
};

template<typename... Ts> class ControlsTitleAction : public Action<Ts...>, public Parented<MediaControls> {
 public:
  TEMPLATABLE_VALUE(std::string, title)
  void play(const Ts &...x) override { this->parent_->set_title(this->title_.value(x...)); }
};

}

#endif
