#include "ble_hid_device_select.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::ble_client_hid {

static const char *const TAG = "ble_client_hid.select";
static const char *const NO_DEVICES = "(no devices found)";

void BLEHIDDeviceSelect::setup() {
  this->option_storage_.reserve(BLEClientHID::MAX_FOUND + 1);
  this->option_storage_.emplace_back(NO_DEVICES);
  this->publish_options_();
}

void BLEHIDDeviceSelect::set_devices(const std::vector<FoundBLEDevice> &devices) {
  if (this->option_storage_.capacity() == 0)
    return;
  this->option_storage_.clear();
  for (const auto &device : devices) {
    if (this->option_storage_.size() >= BLEClientHID::MAX_FOUND)
      break;
    uint8_t bda[6];
    for (int i = 0; i < 6; i++)
      bda[i] = static_cast<uint8_t>(device.address >> (40 - 8 * i));
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(bda, mac);
    this->option_storage_.push_back(device.name.empty() ? std::string(mac) : device.name + " (" + mac + ")");
  }
  if (this->option_storage_.empty())
    this->option_storage_.emplace_back(NO_DEVICES);
  this->publish_options_();
}

void BLEHIDDeviceSelect::publish_options_() {
  FixedVector<const char *> options;
  options.init(this->option_storage_.size());
  for (const auto &option : this->option_storage_)
    options.push_back(option.c_str());
  this->traits.set_options(options);
  this->publish_state(this->option_storage_[0]);
}

void BLEHIDDeviceSelect::control(const std::string &value) {
  if (this->parent_ == nullptr || value == NO_DEVICES)
    return;
  for (size_t i = 0; i < this->option_storage_.size(); i++) {
    if (this->option_storage_[i] != value)
      continue;
    ESP_LOGI(TAG, "Pairing with %s", value.c_str());
    this->publish_state(value);
    this->parent_->pair_with(i);
    return;
  }
}

void BLEHIDDeviceSelect::dump_config() { LOG_SELECT("", "BLE HID device", this); }

}  // namespace esphome::ble_client_hid

#endif  // USE_ESP32
