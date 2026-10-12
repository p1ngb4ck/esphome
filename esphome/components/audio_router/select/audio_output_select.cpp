#include "audio_output_select.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::audio_router {

static const char *const TAG = "audio_router.select";

void AudioOutputSelect::setup() {
  this->parent_->add_on_output_callback([this](size_t index) { this->publish_state(index); });
  this->publish_state(this->parent_->active_output());
}

void AudioOutputSelect::dump_config() { LOG_SELECT("", "Audio output", this); }

void AudioOutputSelect::control(size_t index) { this->parent_->select_output(index); }

}  // namespace esphome::audio_router

#endif  // USE_ESP32
