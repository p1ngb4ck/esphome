#include "output_select.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::resampler {

static const char *const TAG = "resampler.select";

void OutputSelect::setup() {
  this->parent_->add_on_output_callback([this](size_t index) { this->publish_state(index); });
  this->publish_state(this->parent_->active_output());
}

void OutputSelect::dump_config() { LOG_SELECT("", "Output", this); }

void OutputSelect::control(size_t index) { this->parent_->select_output(index); }

}

#endif
