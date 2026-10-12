#include "volume_number.h"

#include "esphome/core/log.h"

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.number";

void VolumeNumber::dump_config() { LOG_NUMBER("", "Simple Video Player Volume", this); }

}  // namespace esphome::simple_video_player
