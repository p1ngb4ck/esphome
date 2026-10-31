#include "ble_broadcast.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstring>

namespace esphome::ble_broadcast {

static const char *const TAG = "ble_broadcast";

static uint64_t to_uint64(const uint8_t *bda) {
  uint64_t address = 0;
  for (int i = 0; i < 6; i++)
    address |= static_cast<uint64_t>(bda[i]) << ((5 - i) * 8);
  return address;
}

static uint16_t uuid16_of(const ble_device_base::ESPBTUUID &uuid) {
  return uuid.type() == ble_device_base::ESPBTUUID::Type::UUID16 ? uuid.uuid16() : 0;
}

void BLEBroadcast::setup() {
  this->pairing_->add_target(this);
  this->pairing_->add_on_forget_callback([this](const bt_pairing::PairingRecord &record) {
    if (record.slot != this->slot_)
      return;
    this->set_address_(nullptr);
    if (this->address_set_ != nullptr)
      this->address_set_("");
  });
}

// The slot's device, once: from the address global, or the pairing database once it is loaded.
void BLEBroadcast::loop() {
  uint8_t bda[6];
  if (this->address_get_ != nullptr && bt_pairing::parse_address(this->address_get_(), bda)) {
    this->set_address_(bda);
  } else if (!this->pairing_->is_loaded()) {
    return;
  } else if (const auto *record = this->pairing_->find_slot(this->slot_); record != nullptr) {
    memcpy(bda, record->address, sizeof(bda));
    this->set_address_(bda);
  }
  if (this->address_ != 0) {
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(bda, mac);
    ESP_LOGV(TAG, "[%s] Listening", mac);
  }
  this->disable_loop();
}

void BLEBroadcast::dump_config() { ESP_LOGCONFIG(TAG, "BLE broadcast device (set by a bt_pairing pick)"); }

void BLEBroadcast::set_address_(const uint8_t *bda) {
  this->address_ = bda == nullptr ? 0 : to_uint64(bda);
  this->last_hash_ = 0;
}

bool BLEBroadcast::pair_device(const bt_pairing::ScannedDevice &device) {
  this->set_address_(device.address);
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(device.address, mac);
  if (this->address_set_ != nullptr)
    this->address_set_(mac);
  bt_pairing::PairingRecord record{};
  memcpy(record.address, device.address, sizeof(record.address));
  record.transport = static_cast<uint8_t>(bt_pairing::Transport::LE);
  record.address_type = device.address_type;
  record.slot = this->slot_;
  strncpy(record.name, device.name.c_str(), sizeof(record.name) - 1);
  this->pairing_->store(record);
  ESP_LOGI(TAG, "[%s] Listening to '%s'", mac, device.name.c_str());
  return true;
}

void BLEBroadcast::unpair() {
  if (this->pairing_->find_slot(this->slot_) != nullptr) {
    this->pairing_->forget_slot(this->slot_);
    return;
  }
  this->set_address_(nullptr);
  if (this->address_set_ != nullptr)
    this->address_set_("");
}

bool BLEBroadcast::parse_device(const ble_device_base::ESPBTDevice &device) {
  if (this->address_ == 0 || device.address_uint64() != this->address_)
    return false;
  const uint32_t hash = bt_pairing::advertisement_hash(device);
  if (hash == this->last_hash_)
    return true;
  this->last_hash_ = hash;
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  device.address_str_to(mac);
  [[maybe_unused]] char hex[format_hex_pretty_size(64)];
  for (const auto &data : device.get_manufacturer_datas()) {
    const uint16_t id = uuid16_of(data.uuid);
    ESP_LOGV(TAG, "[%s] Manufacturer 0x%04X: %s", mac, id, format_hex_pretty_to(hex, data.data, ' '));
    this->manufacturer_cb_.call(data.data, id);
  }
  for (const auto &data : device.get_service_datas()) {
    const uint16_t uuid = uuid16_of(data.uuid);
    ESP_LOGV(TAG, "[%s] Service 0x%04X: %s", mac, uuid, format_hex_pretty_to(hex, data.data, ' '));
    this->service_cb_.call(data.data, uuid);
  }
  return true;
}

}  // namespace esphome::ble_broadcast

#endif  // USE_ESP32
