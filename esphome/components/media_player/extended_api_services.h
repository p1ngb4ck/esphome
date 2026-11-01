#pragma once

#include "esphome/core/defines.h"
#ifdef USE_MEDIA_PLAYER_EXTENDED_API

#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "media_player.h"

#ifdef USE_MEDIA_PLAYER_COVER
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/relay_link/relay_link.h"
#endif

#include <atomic>
#include <string>

namespace esphome::media_player {

class ExtendedApiServices : public Component, public api::CustomAPIDevice {
 public:
  static ExtendedApiServices *create();

  void set_media_player(MediaPlayer *player) { this->player_ = player; }
  void set_status(text_sensor::TextSensor *status) { this->status_ = status; }
#ifdef USE_MEDIA_PLAYER_COVER
  void set_relay(relay_link::RelayLink *relay) { this->relay_ = relay; }
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
  void add_cover_canvas(lv_obj_t *canvas) {
    if (this->cover_view_count_ < COVER_VIEWS)
      this->cover_views_[this->cover_view_count_++].canvas = canvas;
  }
  bool has_cover() const { return this->cover_shown_; }
#endif

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void on_play_audio(std::string url, bool announce);
  void on_audio_command(std::string command);
  void on_set_volume(float volume);
#ifdef USE_MEDIA_PLAYER_COVER
  void on_cover();
#endif

 protected:
  void publish_status_();
  void build_traits_();

  MediaPlayer *player_{nullptr};
  text_sensor::TextSensor *status_{nullptr};
  char traits_[160]{};

#ifdef USE_MEDIA_PLAYER_COVER
  struct CoverView {
    lv_obj_t *canvas{nullptr};
    int32_t w{0};
    int32_t h{0};
    uint8_t *data{nullptr};
    size_t capacity{0};
    lv_draw_buf_t buf{};
    int32_t shown_w{0};
    int32_t shown_h{0};
    uint32_t stride{0};
  };
  static constexpr size_t COVER_VIEWS = 3;
  static constexpr size_t COVER_FILE_MAX = 512 * 1024;
  enum class CoverStep : uint8_t { IDLE, FETCHING };
  static void cover_job_(void *ctx, uint16_t gen);
  void cover_task_(uint16_t gen);
  bool cover_decode_(uint8_t *file, size_t len);
  void cover_sizes_();
  void cover_detach_(CoverView &view);
  void cover_clear_views_();
  void cover_release_();
  CoverView &cover_current_() { return this->cover_views_[this->cover_order_[this->cover_view_]]; }

  relay_link::RelayLink *relay_{nullptr};
  lvgl::LvglComponent *lvgl_{nullptr};
  CoverView cover_views_[COVER_VIEWS];
  size_t cover_view_count_{0};
  CoverStep cover_step_{CoverStep::IDLE};
  size_t cover_view_{0};
  uint8_t cover_order_[COVER_VIEWS]{};
  bool cover_wanted_{false};
  bool cover_shown_{false};
  bool cover_sized_{false};
  uint8_t *cover_file_{nullptr};
  int32_t cover_fetch_w_{0};
  int32_t cover_fetch_h_{0};
  relay_link::RelayLink::Fetch cover_fetch_;
  std::atomic<bool> cover_decode_ok_{false};
#endif
};

}

#endif
