#include "bt_pairing.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <sdkconfig.h>
#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include <esp_bt_main.h>
#ifdef CONFIG_BT_CLASSIC_ENABLED
#include <esp_gap_bt_api.h>
#endif
#ifdef CONFIG_BT_BLE_ENABLED
#include <esp_gap_ble_api.h>
#endif
#endif

#include <cinttypes>
#include <cstring>

namespace esphome::bt_pairing {

static const char *const TAG = "bt_pairing";

static constexpr uint32_t MAGIC = 0x44505442;  // "BTPD"
static constexpr uint8_t VERSION = 1;
static constexpr uint32_t RETRY_MS = 5000;

struct Header {
  uint32_t magic;
  uint8_t version;
  uint8_t count;
  uint16_t crc;
};
static_assert(sizeof(Header) == 8, "Header is a storage format");

bool parse_address(const std::string &text, uint8_t *address) {
  if (text.size() != 17)
    return false;
  for (size_t i = 0; i < 6; i++) {
    if (i > 0 && text[i * 3 - 1] != ':')
      return false;
    if (parse_hex(text.c_str() + i * 3, 2, address + i, 1) != 2)
      return false;
  }
  return true;
}

static const char *transport_str(uint8_t transport) {
  return transport == static_cast<uint8_t>(Transport::LE) ? "LE" : "Classic";
}

void BTPairing::setup() {
  this->pref_ = global_preferences->make_preference(sizeof(Header) + this->max_devices_ * sizeof(PairingRecord),
                                                    this->preference_key_);
}

void BTPairing::loop() {
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->next_attempt_ms_) < 0)
    return;
  if (!this->loaded_) {
    switch (this->load_()) {
      case LoadResult::OK:
        this->loaded_ = true;
        ESP_LOGI(TAG, "%u paired device(s) loaded", static_cast<unsigned>(this->records_.size()));
        this->log_records();
        break;
      case LoadResult::EMPTY:
        this->loaded_ = true;
        ESP_LOGI(TAG, "No paired devices stored");
        break;
      case LoadResult::RETRY:
        this->next_attempt_ms_ = now + RETRY_MS;
        return;
    }
  }
  if (this->dirty_ && !this->save_()) {
    this->next_attempt_ms_ = now + RETRY_MS;
    return;
  }
  this->disable_loop();
}

void BTPairing::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Bluetooth pairing database:\n"
                "  Max devices: %u",
                this->max_devices_);
#ifdef USE_BT_PAIRING_STORAGE
  if (this->file_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  File: %s", this->file_);
    return;
  }
#endif
  ESP_LOGCONFIG(TAG, "  Stored in preferences");
}

const PairingRecord *BTPairing::find_slot(uint32_t slot) const {
  for (const auto &record : this->records_) {
    if (record.slot == slot)
      return &record;
  }
  return nullptr;
}

const PairingRecord *BTPairing::find_address(const uint8_t *address) const {
  for (const auto &record : this->records_) {
    if (memcmp(record.address, address, sizeof(record.address)) == 0)
      return &record;
  }
  return nullptr;
}

bool BTPairing::store(const PairingRecord &record) {
  if (!this->loaded_) {
    ESP_LOGW(TAG, "Database not loaded yet, not stored");
    return false;
  }
  PairingRecord copy = record;
  copy.name[sizeof(copy.name) - 1] = '\0';
  bool replaced = false;
  for (size_t i = 0; i < this->records_.size();) {
    auto &existing = this->records_[i];
    const bool same_slot = existing.slot == copy.slot;
    const bool same_address = memcmp(existing.address, copy.address, sizeof(copy.address)) == 0;
    if (!same_slot && !same_address) {
      i++;
      continue;
    }
    if (!replaced) {
      existing = copy;
      replaced = true;
      i++;
    } else {
      this->records_.erase(this->records_.begin() + i);
    }
  }
  if (!replaced) {
    if (this->records_.size() >= this->max_devices_) {
      ESP_LOGW(TAG, "Database full (%u devices), not stored", this->max_devices_);
      return false;
    }
    this->records_.push_back(copy);
  }
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(copy.address, mac);
  ESP_LOGI(TAG, "Stored %s %s '%s'", transport_str(copy.transport), mac, copy.name);
  this->dirty_ = true;
  this->next_attempt_ms_ = 0;
  this->enable_loop();
  return true;
}

void BTPairing::forget_index_(size_t index) {
  const PairingRecord record = this->records_[index];
  this->records_.erase(this->records_.begin() + index);
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(record.address, mac);
  ESP_LOGI(TAG, "Forgetting %s %s '%s'", transport_str(record.transport), mac, record.name);
  this->forget_callbacks_.call(record);
#ifdef CONFIG_BT_BLUEDROID_ENABLED
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    uint8_t bda[6];
    memcpy(bda, record.address, sizeof(bda));
    if (record.transport == static_cast<uint8_t>(Transport::LE)) {
#ifdef CONFIG_BT_BLE_ENABLED
      esp_ble_remove_bond_device(bda);
#endif
    } else {
#ifdef CONFIG_BT_CLASSIC_ENABLED
      esp_bt_gap_remove_bond_device(bda);
#endif
    }
  } else {
    ESP_LOGW(TAG, "Bluetooth is not running, the bond of %s stays", mac);
  }
#endif
  this->dirty_ = true;
  this->next_attempt_ms_ = 0;
  this->enable_loop();
}

void BTPairing::forget_slot(uint32_t slot) {
  if (!this->loaded_)
    return;
  for (size_t i = 0; i < this->records_.size(); i++) {
    if (this->records_[i].slot == slot) {
      this->forget_index_(i);
      return;
    }
  }
}

bool BTPairing::forget_address(const uint8_t *address) {
  if (!this->loaded_) {
    ESP_LOGW(TAG, "Database not loaded yet");
    return false;
  }
  for (size_t i = 0; i < this->records_.size(); i++) {
    if (memcmp(this->records_[i].address, address, sizeof(this->records_[i].address)) == 0) {
      this->forget_index_(i);
      return true;
    }
  }
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(address, mac);
  ESP_LOGW(TAG, "%s is not in the database", mac);
  return false;
}

void BTPairing::forget_all() {
  if (!this->loaded_) {
    ESP_LOGW(TAG, "Database not loaded yet");
    return;
  }
  while (!this->records_.empty())
    this->forget_index_(this->records_.size() - 1);
}

void BTPairing::log_records() const {
  if (this->records_.empty()) {
    ESP_LOGI(TAG, "No paired devices");
    return;
  }
  for (const auto &record : this->records_) {
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(record.address, mac);
    ESP_LOGI(TAG, "  %s %s '%s', class 0x%06" PRIX32 ", slot 0x%08" PRIX32, transport_str(record.transport), mac,
             record.name, record.device_class, record.slot);
  }
}

std::vector<uint8_t> BTPairing::encode_(bool fixed_size) const {
  const size_t count = fixed_size ? this->max_devices_ : this->records_.size();
  std::vector<uint8_t> out(sizeof(Header) + count * sizeof(PairingRecord), 0);
  const size_t used = this->records_.size() * sizeof(PairingRecord);
  if (used != 0)
    memcpy(out.data() + sizeof(Header), this->records_.data(), used);
  Header header{MAGIC, VERSION, static_cast<uint8_t>(this->records_.size()), 0};
  header.crc = crc16(out.data() + sizeof(Header), static_cast<uint16_t>(used));
  memcpy(out.data(), &header, sizeof(header));
  return out;
}

bool BTPairing::decode_(const uint8_t *data, size_t length) {
  Header header{};
  if (length < sizeof(header))
    return false;
  memcpy(&header, data, sizeof(header));
  const size_t used = header.count * sizeof(PairingRecord);
  if (header.magic != MAGIC || header.version != VERSION || sizeof(header) + used > length ||
      crc16(data + sizeof(header), static_cast<uint16_t>(used)) != header.crc)
    return false;
  this->records_.clear();
  for (size_t i = 0; i < header.count && i < this->max_devices_; i++) {
    PairingRecord record;
    memcpy(&record, data + sizeof(header) + i * sizeof(PairingRecord), sizeof(record));
    record.name[sizeof(record.name) - 1] = '\0';
    this->records_.push_back(record);
  }
  if (header.count > this->max_devices_)
    ESP_LOGW(TAG, "%u stored devices, max_devices %u: the rest are dropped", header.count, this->max_devices_);
  return true;
}

#ifdef USE_BT_PAIRING_STORAGE
storage::PathStorage *BTPairing::resolve_file_(const char **rel) {
  if (storage::global_storage_registry == nullptr)
    return nullptr;
  storage::PathStorage *storage = storage::global_storage_registry->resolve_path(this->file_, rel);
  if (storage == nullptr)
    ESP_LOGD(TAG, "No storage mounted for %s yet", this->file_);
  return storage;
}
#endif

BTPairing::LoadResult BTPairing::load_() {
#ifdef USE_BT_PAIRING_STORAGE
  if (this->file_ != nullptr) {
    const char *rel = nullptr;
    storage::PathStorage *storage = this->resolve_file_(&rel);
    if (storage == nullptr)
      return LoadResult::RETRY;
    storage::RamBuffer buffer;
    size_t size = 0;
    const storage::StorageError err = storage::read_file(storage, rel, buffer, &size);
    if (err == storage::StorageError::STORAGE_ERROR_NOT_FOUND)
      return LoadResult::EMPTY;
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      ESP_LOGD(TAG, "%s not readable yet (%u), retrying", this->file_, static_cast<unsigned>(err));
      return LoadResult::RETRY;
    }
    if (!this->decode_(buffer.get(), size)) {
      ESP_LOGW(TAG, "%s is not a pairing database, starting empty", this->file_);
      return LoadResult::EMPTY;
    }
    return LoadResult::OK;
  }
#endif
  std::vector<uint8_t> data(sizeof(Header) + this->max_devices_ * sizeof(PairingRecord));
  if (!this->pref_.load(data.data(), data.size()) || !this->decode_(data.data(), data.size()))
    return LoadResult::EMPTY;
  return LoadResult::OK;
}

bool BTPairing::save_() {
#ifdef USE_BT_PAIRING_STORAGE
  if (this->file_ != nullptr) {
    const char *rel = nullptr;
    storage::PathStorage *storage = this->resolve_file_(&rel);
    if (storage == nullptr)
      return false;
    const auto data = this->encode_(false);
    const storage::StorageError err = storage::write_file(storage, rel, data.data(), data.size());
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      ESP_LOGW(TAG, "Writing %s failed (%u), retrying", this->file_, static_cast<unsigned>(err));
      return false;
    }
    this->dirty_ = false;
    return true;
  }
#endif
  const auto data = this->encode_(true);
  if (!this->pref_.save(data.data(), data.size()) || !global_preferences->sync()) {
    ESP_LOGW(TAG, "Saving to preferences failed, retrying");
    return false;
  }
  this->dirty_ = false;
  return true;
}

}  // namespace esphome::bt_pairing

#endif  // USE_ESP32
