#pragma once

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "../simple_video_player.h"

namespace esphome::simple_video_player {

// The player's status for svp_relay, as JSON (SimpleVideoPlayer::status_json()).
class StatusTextSensor : public text_sensor::TextSensor, public Component {
 public:
  void set_parent(SimpleVideoPlayer *parent) { this->parent_ = parent; }
  void setup() override {
    this->parent_->add_on_status_callback([this]() { this->publish_state(this->parent_->status_json()); });
    this->publish_state(this->parent_->status_json());
  }
  void dump_config() override;

 protected:
  SimpleVideoPlayer *parent_{nullptr};
};

}  // namespace esphome::simple_video_player
