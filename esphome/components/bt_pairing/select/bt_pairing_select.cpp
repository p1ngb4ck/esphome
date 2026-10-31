#include "bt_pairing_select.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

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
  std::vector<std::string> wanted;
  wanted.reserve(std::min(devices.size(), BTPairing::MAX_SCANNED) + 1);
  wanted.emplace_back(devices.empty() ? NO_DEVICES : PICK);
  for (const auto &device : devices) {
    if (wanted.size() > BTPairing::MAX_SCANNED)
      break;
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(device.address, mac);
    const char *kind = device.kind == DeviceKind::BROADCAST ? "[BLE broadcast] "
                       : device.transport == Transport::LE  ? "[BLE HID] "
                       : device.kind == DeviceKind::AUDIO   ? "[BT audio] "
                                                            : "[BT HID] ";
    wanted.push_back(kind + (device.name.empty() ? std::string(mac) : device.name + " (" + mac + ")"));
  }
  // Slot by slot, so a client gets only what changed. The storage never grows past its reserve,
  // so the strings the traits point at stay where they are.
  auto &storage = this->option_storage_;
  const bool placeholder_changed = storage[0] != wanted[0];
  bool changed = false;
  for (size_t i = 0; i < wanted.size() && i < storage.size(); i++) {
    if (storage[i] == wanted[i])
      continue;
    storage[i] = std::move(wanted[i]);
    this->traits.set_option(i, storage[i].c_str());
    changed = true;
  }
  for (size_t i = storage.size(); i < wanted.size(); i++) {
    storage.push_back(std::move(wanted[i]));
    this->traits.add_option(storage.back().c_str());
    changed = true;
  }
  while (storage.size() > wanted.size()) {
    this->traits.remove_option(storage.size() - 1);
    storage.pop_back();
    changed = true;
  }
  if (placeholder_changed || this->active_index().value_or(1) != 0) {
    this->publish_state(size_t{0});
  } else if (changed) {
    this->publish_options();
  }
}

void BTPairingSelect::publish_options_() {
  FixedVector<const char *> options;
  options.init(this->option_storage_.size());
  for (const auto &option : this->option_storage_)
    options.push_back(option.c_str());
  this->traits.set_options(options);
  this->publish_state(size_t{0});
}

void BTPairingSelect::control(const std::string &value) {
  if (this->parent_ == nullptr)
    return;
  // Option 0 is the placeholder; option i is device i - 1.
  for (size_t i = 1; i < this->option_storage_.size(); i++) {
    if (this->option_storage_[i] != value)
      continue;
    ESP_LOGI(TAG, "Picked %s", value.c_str());
    // Back to the placeholder, so the same device can be picked again.
    this->publish_state(size_t{0});
    this->parent_->pair_with(i - 1);
    return;
  }
}

void BTPairingSelect::dump_config() { LOG_SELECT("", "Bluetooth pairing device", this); }

}  // namespace esphome::bt_pairing

#endif  // USE_ESP32
