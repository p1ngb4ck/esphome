#pragma once

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "../simple_video_player.h"

#ifdef USE_SPEAKER

namespace esphome::simple_video_player {

// The volume of what the player plays (files and streams): the speaker's own, in percent.
class VolumeNumber : public number::Number, public Component {
 public:
  void set_parent(SimpleVideoPlayer *parent) { this->parent_ = parent; }
  void setup() override { this->publish_state(this->parent_->get_volume() * 100.0f); }
  void dump_config() override;

 protected:
  void control(float value) override {
    this->parent_->set_volume(value / 100.0f);
    this->publish_state(value);
  }

  SimpleVideoPlayer *parent_{nullptr};
};

}  // namespace esphome::simple_video_player

#endif  // USE_SPEAKER
