#include "source_select.h"

#if defined(USE_ESP32) && defined(USE_RESAMPLER_SOURCE_SELECT)

#include "esphome/core/log.h"

namespace esphome::resampler {

static const char *const TAG = "resampler.source_select";

void SourceSelect::setup() {
  this->parent_->add_on_source_callback([this](int index) { this->publish_source_(index); });
  this->parent_->add_on_sources_changed_callback([this]() { this->publish_options_(); });
  this->publish_options_();
}

void SourceSelect::publish_options_() {
  FixedVector<const char *> options;
  options.init(this->parent_->source_count());
  for (size_t i = 0; i < this->parent_->source_count(); i++)
    options.push_back(this->parent_->source_name(i));
  this->traits.set_options(options);
  this->publish_source_(this->parent_->get_source());
}

void SourceSelect::publish_source_(int index) {
  if (index >= 0) {
    this->publish_state(static_cast<size_t>(index));
  } else {
    this->publish_options();
  }
}

void SourceSelect::dump_config() { LOG_SELECT("", "Source", this); }

void SourceSelect::control(size_t index) { this->parent_->set_source(static_cast<int>(index)); }

}  // namespace esphome::resampler

#endif
