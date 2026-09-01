#include "a2dp_device_select.h"

#ifdef USE_ESP32_VARIANT_ESP32

#include "../a2dp_source.h"
#include "esphome/core/log.h"

namespace esphome {
namespace a2dp_source {

static const char *const TAG = "a2dp_source.select";

// Shown while no pairing window has run, so the entity has something to display
// rather than an empty list that looks broken.
static const char *const NO_DEVICES = "(no devices found)";

void A2DPDeviceSelect::setup() {
  // Reserved once and never grown again. Everything below hands the traits bare
  // pointers into these strings, and a reallocation would leave the select
  // pointing at freed memory -- for short names in particular, which live inside
  // the string object itself and move with it.
  this->option_storage_.reserve(A2DPSource::max_discovered() + 1);
  this->option_storage_.emplace_back(NO_DEVICES);

  FixedVector<const char *> options;
  options.init(1);
  options.push_back(this->option_storage_[0].c_str());
  this->traits.set_options(options);
  this->publish_state(this->option_storage_[0]);
}

void A2DPDeviceSelect::set_devices(const std::vector<DiscoveredDevice> &devices) {
  // The strings are replaced in place, so the vector's capacity is untouched and
  // the pointers below stay inside the same allocation.
  this->option_storage_.clear();
  if (devices.empty()) {
    this->option_storage_.emplace_back(NO_DEVICES);
  } else {
    for (const auto &device : devices) {
      if (this->option_storage_.size() >= this->option_storage_.capacity()) {
        break;
      }
      this->option_storage_.push_back(device.name);
    }
  }

  FixedVector<const char *> options;
  options.init(this->option_storage_.size());
  for (const auto &option : this->option_storage_) {
    options.push_back(option.c_str());
  }
  this->traits.set_options(options);
  this->publish_state(this->option_storage_[0]);
}

void A2DPDeviceSelect::control(const std::string &value) {
  if (this->parent_ == nullptr || value == NO_DEVICES) {
    return;
  }
  // Publish first: the pairing itself completes later, when the inquiry reports
  // the chosen device again, and the entity should show what was picked in the
  // meantime rather than snapping back.
  this->publish_state(value);
  this->parent_->pair_with_name(value);
}

void A2DPDeviceSelect::dump_config() { LOG_SELECT("", "A2DP device", this); }

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32
