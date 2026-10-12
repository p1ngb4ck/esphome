#pragma once

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "../simple_video_player.h"

#include <cmath>

namespace esphome::simple_video_player {

// The device's volume (SimpleVideoPlayer::set_volume), in percent; follows changes made elsewhere.
class VolumeNumber : public number::Number, public Component {
 public:
  void set_parent(SimpleVideoPlayer *parent) { this->parent_ = parent; }
  void setup() override {
    this->parent_->add_on_status_callback([this]() { this->follow_(); });
    this->publish_state(this->parent_->get_volume() * 100.0f);
  }
  void dump_config() override;

 protected:
  void control(float value) override {
    this->parent_->set_volume(value / 100.0f);
    this->publish_state(value);
  }

  void follow_() {
    float value = roundf(this->parent_->get_volume() * 100.0f);
    if (!this->has_state() || value != this->state)
      this->publish_state(value);
  }

  SimpleVideoPlayer *parent_{nullptr};
};

}  // namespace esphome::simple_video_player
