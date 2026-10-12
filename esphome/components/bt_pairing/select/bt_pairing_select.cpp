#include "bt_pairing_select.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::bt_pairing {

static const char *const TAG = "bt_pairing.select";
static const char *const NO_DEVICES = "(no devices found)";
static const char *const PICK = "(pick a device)";

void BTPairingSelect::setup() {
  this->option_storage_.reserve(BTPairing::MAX_SCANNED + 1);
  this->option_storage_.emplace_back(NO_DEVICES);
  this->publish_options_();
}

void BTPairingSelect::set_devices(const std::vector<ScannedDevice> &devices) {
  if (this->option_storage_.capacity() == 0)
    return;
  this->option_storage_.clear();
  this->option_storage_.emplace_back(devices.empty() ? NO_DEVICES : PICK);
  for (const auto &device : devices) {
    if (this->option_storage_.size() > BTPairing::MAX_SCANNED)
      break;
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(device.address, mac);
    const char *kind = device.transport == Transport::LE  ? "[BLE HID] "
                       : device.kind == DeviceKind::AUDIO ? "[BT audio] "
                                                          : "[BT HID] ";
    this->option_storage_.push_back(kind + (device.name.empty() ? std::string(mac) : device.name + " (" + mac + ")"));
  }
  this->publish_options_();
}

void BTPairingSelect::publish_options_() {
  FixedVector<const char *> options;
  options.init(this->option_storage_.size());
  for (const auto &option : this->option_storage_)
    options.push_back(option.c_str());
  this->traits.set_options(options);
  this->publish_state(this->option_storage_[0]);
}

void BTPairingSelect::control(const std::string &value) {
  if (this->parent_ == nullptr)
    return;
  // Option 0 is the placeholder; option i is device i - 1.
  for (size_t i = 1; i < this->option_storage_.size(); i++) {
    if (this->option_storage_[i] != value)
      continue;
    ESP_LOGI(TAG, "Picked %s", value.c_str());
    this->publish_state(value);
    this->parent_->pair_with(i - 1);
    return;
  }
}

void BTPairingSelect::dump_config() { LOG_SELECT("", "Bluetooth pairing device", this); }

}  // namespace esphome::bt_pairing

#endif  // USE_ESP32
