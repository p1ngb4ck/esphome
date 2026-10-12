#include "status_text_sensor.h"
#include "esphome/core/log.h"

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.text_sensor";

void StatusTextSensor::dump_config() { LOG_TEXT_SENSOR("", "Simple Video Player Status", this); }

}  // namespace esphome::simple_video_player
