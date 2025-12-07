#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/transcoder/transcoder.h"
#include "lvgl.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace esphome::simple_video_player {

class SimpleVideoPlayer : public Component {
 public:
  SimpleVideoPlayer() = default;
  ~SimpleVideoPlayer();

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_transcoder(transcoder::Transcoder *tc) { this->transcoder_ = tc; }
  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }

 protected:
  transcoder::Transcoder *transcoder_{nullptr};
  lv_obj_t *canvas_{nullptr};
};

}  // namespace esphome::simple_video_player
