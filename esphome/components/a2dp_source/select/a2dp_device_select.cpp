#include "a2dp_device_select.h"

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#include "../a2dp_source.h"
#include "esphome/core/log.h"

#include <cstdio>

namespace esphome {
namespace a2dp_source {

static const char *const TAG = "a2dp_source.select";

static const char *const NO_DEVICES = "(no devices found)";
static const char *const PICK = "(pick a device)";

void A2DPDeviceSelect::setup() {
  this->option_storage_.reserve(A2DPSource::max_discovered() + 1);
  this->option_storage_.emplace_back(NO_DEVICES);

  FixedVector<const char *> options;
  options.init(1);
  options.push_back(this->option_storage_[0].c_str());
  this->traits.set_options(options);
  this->publish_state(this->option_storage_[0]);
}

void A2DPDeviceSelect::set_devices(const std::vector<DiscoveredDevice> &devices) {
  this->option_storage_.clear();
  // Option 0 is a placeholder, so even a single device is a change to pick.
  this->option_storage_.emplace_back(devices.empty() ? NO_DEVICES : PICK);
  for (const auto &device : devices) {
    if (this->option_storage_.size() >= this->option_storage_.capacity()) {
      break;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", device.address[0], device.address[1], device.address[2],
             device.address[3], device.address[4], device.address[5]);
    this->option_storage_.push_back(device.name.empty() ? std::string(mac) : device.name + " (" + mac + ")");
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
  if (this->parent_ == nullptr) {
    return;
  }
  for (size_t i = 1; i < this->option_storage_.size(); i++) {
    if (this->option_storage_[i] != value)
      continue;
    // Back to the placeholder, so the same device can be picked again.
    this->publish_state(this->option_storage_[0]);
    this->parent_->pair_with_index(i - 1);
    return;
  }
}

void A2DPDeviceSelect::dump_config() { LOG_SELECT("", "A2DP device", this); }

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32 || USE_A2DP_SOURCE_EXTERNAL_HOST
