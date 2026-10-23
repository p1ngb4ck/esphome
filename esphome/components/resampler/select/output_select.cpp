#include "output_select.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::resampler {

static const char *const TAG = "resampler.select";

void OutputSelect::setup() {
  this->parent_->add_on_output_callback([this](size_t index) { this->publish_state(index); });
  this->parent_->add_on_outputs_changed_callback([this]() { this->publish_options_(); });
  this->publish_options_();
}

void OutputSelect::publish_options_() {
  FixedVector<const char *> options;
  options.init(this->parent_->output_count());
  for (size_t i = 0; i < this->parent_->output_count(); i++)
    options.push_back(this->parent_->output_name(i));
  this->traits.set_options(options);
  if (this->parent_->output_count() != 0) {
    this->publish_state(this->parent_->active_output());
  } else {
    this->publish_options();
  }
}

void OutputSelect::dump_config() { LOG_SELECT("", "Output", this); }

void OutputSelect::control(size_t index) { this->parent_->select_output(index); }

}

#endif
