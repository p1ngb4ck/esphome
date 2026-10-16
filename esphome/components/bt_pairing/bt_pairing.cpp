#include "bt_pairing.h"

#ifdef USE_ESP32

#ifdef USE_BT_PAIRING_SELECT
#include "select/bt_pairing_select.h"
#endif
#include "esphome/core/application.h"
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

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::bt_pairing {

static const char *const TAG = "bt_pairing";

static constexpr uint32_t MAGIC = 0x44505442;  // "BTPD"
static constexpr uint8_t VERSION = 1;
static constexpr uint32_t RETRY_MS = 5000;
// The preferences blob has room for the most max_devices allows, so changing max_devices
// keeps the stored devices.
static constexpr size_t PREFERENCE_RECORDS = 32;

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

static const char *kind_str(const ScannedDevice &device) {
  if (device.transport == Transport::LE)
    return device.kind == DeviceKind::BROADCAST ? "BLE broadcast" : "BLE HID";
  return device.kind == DeviceKind::AUDIO ? "BT audio" : "BT HID";
}

void BTPairing::setup() {
  this->pref_ = global_preferences->make_preference(sizeof(Header) + PREFERENCE_RECORDS * sizeof(PairingRecord),
                                                    this->preference_key_);
#ifdef USE_BT_CLASSIC
  bt_classic::add_gap_handler(BTPairing::gap_handler_, this);
#endif
}

void BTPairing::loop() {
  this->scan_loop_();
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->next_attempt_ms_) < 0)
    return;
  if (!this->loaded_) {
    switch (this->load_()) {
      case LoadResult::OK:
        this->loaded_ = true;
        ESP_LOGI(TAG, "%u paired device(s) loaded", static_cast<unsigned>(this->records_.size()));
        this->log_records();
#ifdef USE_BT_PAIRING_HID
        this->devices_changed_();
#endif
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
  if (!this->scanning_.load() && this->pending_target_ == nullptr)
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
  this->next_attempt_ms_ = millis();
  this->enable_loop();
#ifdef USE_BT_PAIRING_HID
  this->devices_changed_();
#endif
  return true;
}

void BTPairing::forget_index_(size_t index) {
  const PairingRecord record = this->records_[index];
  this->records_.erase(this->records_.begin() + index);
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(record.address, mac);
  ESP_LOGI(TAG, "Forgetting %s %s '%s'", transport_str(record.transport), mac, record.name);
  this->forget_callbacks_.call(record);
  delete_bond(record.address, static_cast<Transport>(record.transport));
  this->dirty_ = true;
  this->next_attempt_ms_ = millis();
  this->enable_loop();
#ifdef USE_BT_PAIRING_HID
  this->devices_changed_();
#endif
}

void BTPairing::delete_bond(const uint8_t *address, Transport transport) {
#ifdef CONFIG_BT_BLUEDROID_ENABLED
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(address, mac);
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    ESP_LOGW(TAG, "Bluetooth is not running, the bond of %s stays", mac);
    return;
  }
  uint8_t bda[6];
  memcpy(bda, address, sizeof(bda));
  esp_err_t err = ESP_ERR_NOT_SUPPORTED;
  if (transport == Transport::LE) {
#ifdef CONFIG_BT_BLE_ENABLED
    err = esp_ble_remove_bond_device(bda);
#endif
  } else {
#ifdef CONFIG_BT_CLASSIC_ENABLED
    err = esp_bt_gap_remove_bond_device(bda);
#endif
  }
  ESP_LOGD(TAG, "Bond of %s deleted: %s", mac, esp_err_to_name(err));
#endif
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
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  if (this->records_.empty()) {
    ESP_LOGI(TAG, "No paired devices");
  } else {
    ESP_LOGI(TAG, "Paired devices:");
  }
  for (const auto &record : this->records_) {
    format_mac_addr_upper(record.address, mac);
    const PairingTarget *target = this->target_of_(record.address);
    ESP_LOGI(TAG, "  %s %s '%s', class 0x%06" PRIX32 ", %s", transport_str(record.transport), mac, record.name,
             record.device_class,
             target == nullptr               ? "no slot"
             : target->is_device_connected() ? "connected"
                                             : "disconnected");
  }
  if (this->scanned_.empty())
    return;
  ESP_LOGI(TAG, "Found by the last scan:");
  for (const auto &device : this->scanned_) {
    format_mac_addr_upper(device.address, mac);
    ESP_LOGI(TAG, "  %s %s '%s'", kind_str(device), mac, device.name.c_str());
  }
}

// -- Pairing scan -------------------------------------------------------------------------

void BTPairing::status_(const std::string &message) {
  ESP_LOGI(TAG, "%s", message.c_str());
  this->status_callbacks_.call(message);
}

#ifdef USE_BT_CLASSIC
// BTC task.
bool BTPairing::gap_handler_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg) {
  auto *self = static_cast<BTPairing *>(arg);
  if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
    self->classic_discovering_.store(param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
    self->enable_loop_soon_any_context();
    return false;
  }
  if (event != ESP_BT_GAP_DISC_RES_EVT || !self->scanning_.load())
    return false;
  uint32_t cod = 0;
  std::string name;
  for (int i = 0; i < param->disc_res.num_prop; i++) {
    const esp_bt_gap_dev_prop_t &prop = param->disc_res.prop[i];
    if (prop.type == ESP_BT_GAP_DEV_PROP_COD && prop.len >= (int) sizeof(uint32_t)) {
      cod = *static_cast<const uint32_t *>(prop.val);
    } else if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME && name.empty() && prop.len > 0) {
      const auto *text = static_cast<const char *>(prop.val);
      name.assign(text, strnlen(text, prop.len));
    } else if (prop.type == ESP_BT_GAP_DEV_PROP_EIR && name.empty()) {
      uint8_t len = 0;
      auto *eir = static_cast<uint8_t *>(prop.val);
      uint8_t *text = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
      if (text == nullptr)
        text = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
      if (text != nullptr)
        name.assign(reinterpret_cast<const char *>(text), len);
    }
  }
  ScannedDevice device{};
  memcpy(device.address, param->disc_res.bda, sizeof(device.address));
  device.transport = Transport::CLASSIC;
  // An inquiry result always has a class; a device without EIR sends its name later without one.
  if (cod == 0 && !name.empty()) {
    device.name = std::move(name);
    LockGuard guard(self->found_lock_);
    if (self->found_.size() < MAX_SCANNED)
      self->found_.push_back(std::move(device));
    self->enable_loop_soon_any_context();
    return false;
  }
  if (!esp_bt_gap_is_valid_cod(cod))
    return false;
  const uint32_t major = esp_bt_gap_get_cod_major_dev(cod);
  if (major == ESP_BT_COD_MAJOR_DEV_PERIPHERAL) {
    device.kind = DeviceKind::HID;
  } else if (major == ESP_BT_COD_MAJOR_DEV_AV &&
             (esp_bt_gap_get_cod_srvc(cod) & (ESP_BT_COD_SRVC_RENDERING | ESP_BT_COD_SRVC_AUDIO)) != 0) {
    // What a2dp_source connects to.
    device.kind = DeviceKind::AUDIO;
  } else {
    return false;
  }
  device.device_class = cod;
  device.name = std::move(name);
  {
    LockGuard guard(self->found_lock_);
    if (self->found_.size() < MAX_SCANNED)
      self->found_.push_back(std::move(device));
  }
  self->enable_loop_soon_any_context();
  return false;
}
#endif

#ifdef USE_BT_PAIRING_LE
// GAP appearance category 0x00F: HID (keyboard, mouse, joystick, gamepad, ...).
static constexpr uint16_t APPEARANCE_HID_FIRST = 0x03C0;
static constexpr uint16_t APPEARANCE_HID_LAST = 0x03FF;
static constexpr uint16_t UUID_HID_SERVICE = 0x1812;

uint32_t advertisement_hash(const ble_device_base::ESPBTDevice &device) {
  uint32_t hash = 2166136261UL;
  auto mix = [&hash](const std::vector<uint8_t> &data) {
    for (uint8_t b : data) {
      hash ^= b;
      hash *= 16777619UL;
    }
  };
  for (const auto &data : device.get_manufacturer_datas())
    mix(data.data);
  for (const auto &data : device.get_service_datas())
    mix(data.data);
  return hash;
}

// A device whose advertised data changes while the scan runs (a button pressed) is a
// broadcast device; one that only repeats itself is not listed.
static bool payload_changed(std::vector<std::pair<uint64_t, uint32_t>> &seen,
                            const ble_device_base::ESPBTDevice &device) {
  static constexpr size_t MAX_TRACKED = 64;
  if (device.get_manufacturer_datas().empty() && device.get_service_datas().empty())
    return false;
  const uint64_t address = device.address_uint64();
  const uint32_t hash = advertisement_hash(device);
  for (const auto &entry : seen) {
    if (entry.first == address)
      return entry.second != hash;
  }
  if (seen.size() < MAX_TRACKED)
    seen.emplace_back(address, hash);
  return false;
}

bool BTPairing::parse_device(const ble_device_base::ESPBTDevice &device) {
  if (!this->scanning_.load())
    return false;
  const auto hid_uuid = ble_device_base::ESPBTUUID::from_uint16(UUID_HID_SERVICE);
  bool hid = false;
  for (const auto &uuid : device.get_service_uuids())
    hid = hid || uuid == hid_uuid;
  const auto &appearance = device.get_appearance();
  if (appearance.has_value() && *appearance >= APPEARANCE_HID_FIRST && *appearance <= APPEARANCE_HID_LAST)
    hid = true;
  DeviceKind kind = DeviceKind::HID;
  if (!hid) {
    if (!this->list_broadcast_ || !payload_changed(this->adv_hashes_, device))
      return false;
    kind = DeviceKind::BROADCAST;
  }
  ScannedDevice found{};
  memcpy(found.address, device.address(), sizeof(found.address));
  found.transport = Transport::LE;
  found.kind = kind;
  found.address_type = static_cast<uint8_t>(device.get_address_type());
  found.device_class = appearance.has_value() ? *appearance : 0;
  const StringRef name = device.get_name();
  if (!name.empty())
    found.name.assign(name.c_str(), name.size());
  this->add_scanned_(found);
  return false;
}
#endif

void BTPairing::scan(uint32_t duration_ms) {
  if (this->scanning_.load()) {
    this->status_("A scan is already running");
    return;
  }
  this->pending_target_ = nullptr;
  this->scanned_.clear();
  this->adv_hashes_.clear();
  this->list_broadcast_ = false;
  for (auto *target : this->targets_)
    this->list_broadcast_ = this->list_broadcast_ || target->pairs(Transport::LE, DeviceKind::BROADCAST);
  {
    LockGuard guard(this->found_lock_);
    this->found_.clear();
  }
  bool started = false;
  this->scanning_.store(true);
#ifdef USE_BT_CLASSIC
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED && bt_classic::ensure_gap_callback()) {
    // Inquiry length is in units of 1.28 s.
    const uint32_t units =
        std::clamp<uint32_t>((duration_ms + 1279) / 1280, ESP_BT_GAP_MIN_INQ_LEN, ESP_BT_GAP_MAX_INQ_LEN);
    if (esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, units, 0); err != ESP_OK) {
      ESP_LOGW(TAG, "Bluetooth Classic scan failed to start: %s", esp_err_to_name(err));
    } else {
      started = true;
    }
  } else {
    ESP_LOGW(TAG, "Bluetooth Classic is not running");
  }
#endif
#ifdef USE_BT_PAIRING_LE
  if (this->parent_ != nullptr) {
    this->parent_->start_scan_if_idle();
    started = true;
  }
#endif
  if (!started) {
    this->scanning_.store(false);
    this->publish_scanned_();
    this->status_("Bluetooth is not running, cannot scan");
    return;
  }
  this->scan_until_ = millis() + duration_ms;
  this->publish_scanned_();
  this->enable_loop();
  this->status_(this->list_broadcast_
                    ? "Scanning for Bluetooth devices in pairing mode; press a button on a BLE broadcast device"
                    : "Scanning for Bluetooth devices in pairing mode");
}

void BTPairing::cancel_scan() {
  const bool active = this->scanning_.exchange(false) || this->pending_target_ != nullptr;
  this->pending_target_ = nullptr;
  for (auto *target : this->targets_)
    target->cancel_device_pairing();
#ifdef USE_BT_CLASSIC
  if (this->classic_discovering_.load())
    esp_bt_gap_cancel_discovery();
#endif
  if (active)
    this->status_("Scan cancelled");
}

void BTPairing::add_scanned_(const ScannedDevice &device) {
  for (auto &known : this->scanned_) {
    if (memcmp(known.address, device.address, sizeof(device.address)) != 0)
      continue;
    if (known.name.empty() && !device.name.empty()) {
      known.name = device.name;
      this->publish_scanned_();
    }
    return;
  }
  // A Classic name-only result names a device listed before
  if ((device.transport == Transport::CLASSIC && device.device_class == 0) || this->scanned_.size() >= MAX_SCANNED)
    return;
  this->scanned_.push_back(device);
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(device.address, mac);
  ESP_LOGI(TAG, "Found %s %s '%s'", kind_str(device), mac, device.name.c_str());
  this->publish_scanned_();
}

void BTPairing::publish_scanned_() {
#ifdef USE_BT_PAIRING_SELECT
  if (this->device_select_ != nullptr)
    this->device_select_->set_devices(this->scanned_);
#endif
}

void BTPairing::scan_loop_() {
  std::deque<ScannedDevice> found;
  {
    LockGuard guard(this->found_lock_);
    found.swap(this->found_);
  }
  for (const auto &device : found)
    this->add_scanned_(device);
  if (this->scanning_.load() && static_cast<int32_t>(millis() - this->scan_until_) >= 0) {
    this->scanning_.store(false);
    char text[96];
    snprintf(text, sizeof(text), "Scan finished: %u device(s), pick one in the device select",
             static_cast<unsigned>(this->scanned_.size()));
    this->status_(text);
  }
  // A Classic device is paged once the inquiry has stopped.
  if (this->pending_target_ != nullptr && !this->classic_discovering_.load()) {
    PairingTarget *target = this->pending_target_;
    this->pending_target_ = nullptr;
    this->dispatch_(this->pending_, target);
  }
}

bool BTPairing::slot_in_use(uint32_t slot) const {
  for (auto *target : this->targets_) {
    if (target->pairing_slot() == slot)
      return true;
  }
  return false;
}

PairingTarget *BTPairing::target_of_(const uint8_t *address) const {
  const PairingRecord *record = this->find_address(address);
  if (record == nullptr || record->slot == 0)
    return nullptr;
  for (auto *target : this->targets_) {
    if (target->pairing_slot() == record->slot)
      return target;
  }
  return nullptr;
}

// The slot already holding the device, else a free one within the HID budget.
PairingTarget *BTPairing::target_for_(const ScannedDevice &device, const char **reason) const {
  if (PairingTarget *target = this->target_of_(device.address);
      target != nullptr && target->pairs(device.transport, device.kind))
    return target;
  if (device.kind == DeviceKind::HID && this->max_hid_devices_ != 0) {
    size_t used = 0;
    for (auto *target : this->targets_) {
      if ((target->pairs(Transport::CLASSIC, DeviceKind::HID) || target->pairs(Transport::LE, DeviceKind::HID)) &&
          !target->is_free())
        used++;
    }
    if (used >= this->max_hid_devices_) {
      *reason = "all HID devices in use";
      return nullptr;
    }
  }
  for (auto *target : this->targets_) {
    if (target->pairs(device.transport, device.kind) && target->is_free())
      return target;
  }
  *reason = "no free slot";
  return nullptr;
}

void BTPairing::pair_with(size_t index) {
  if (index >= this->scanned_.size())
    return;
  const ScannedDevice device = this->scanned_[index];
  char text[128];
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(device.address, mac);
  const char *reason = "";
  PairingTarget *target = this->target_for_(device, &reason);
  if (target == nullptr) {
    snprintf(text, sizeof(text), "Cannot pair %s %s: %s, unpair one first", kind_str(device), mac, reason);
    this->status_(text);
    return;
  }
  this->scanning_.store(false);
#ifdef USE_BT_CLASSIC
  if (device.transport == Transport::CLASSIC && this->classic_discovering_.load()) {
    // Paging during an inquiry fails.
    esp_bt_gap_cancel_discovery();
    this->pending_ = device;
    this->pending_target_ = target;
    this->enable_loop();
    return;
  }
#endif
  this->dispatch_(device, target);
}

void BTPairing::dispatch_(const ScannedDevice &device, PairingTarget *target) {
  char text[128];
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(device.address, mac);
  const char *label = device.name.empty() ? mac : device.name.c_str();
  this->dispatched_ = device;
  this->dispatched_target_ = target;
  if (target->pair_device(device)) {
    snprintf(text, sizeof(text), "Pairing with %s '%s'", kind_str(device), label);
  } else {
    snprintf(text, sizeof(text), "%s '%s' cannot be paired now", kind_str(device), label);
  }
  this->status_(text);
}

std::string BTPairing::device_label(const PairingTarget *target) const {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  if (target == this->dispatched_target_ && !target->is_free()) {
    if (!this->dispatched_.name.empty())
      return this->dispatched_.name;
    format_mac_addr_upper(this->dispatched_.address, mac);
    return mac;
  }
  const PairingRecord *record = target->pairing_slot() != 0 ? this->find_slot(target->pairing_slot()) : nullptr;
  if (record == nullptr)
    return "";
  if (record->name[0] != '\0')
    return record->name;
  format_mac_addr_upper(record->address, mac);
  return mac;
}

void BTPairing::on_pin_request(const PairingTarget *target, const std::string &pin) {
  this->status_("Type " + pin + " + Enter on " + this->device_label(target));
}

void BTPairing::on_passkey(const PairingTarget *target, uint32_t passkey) {
  char text[96];
  snprintf(text, sizeof(text), "Type %06" PRIu32 " + Enter on %s", passkey, this->device_label(target).c_str());
  this->status_(text);
}

void BTPairing::on_connection(const PairingTarget *target, bool connected) {
  const std::string label = this->device_label(target);
  this->status_(label + (connected ? " connected" : " disconnected"));
  this->connection_callbacks_.call(label, connected);
#ifdef USE_BT_PAIRING_HID
  this->devices_changed_();
#endif
}

#ifdef USE_BT_PAIRING_HID
uint8_t BTPairing::get_device_kinds() const {
  uint8_t kinds = 0;
  for (const auto *input : this->hid_slots_)
    kinds |= input->get_device_kinds();
  return kinds;
}

void BTPairing::add_hid_slot(hid::HIDInput *input, PairingTarget *target) {
  this->hid_slots_.push_back(input);
  this->hid_targets_.push_back(target);
  input->add_on_key_event_callback([this, target](uint16_t page, uint16_t usage, bool pressed) {
    this->event_device_ = this->device_label(target);
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
    ESP_LOGD(TAG, "[%s] %s %s", this->event_device_.c_str(), hid::key_name(page, usage).c_str(),
             pressed ? "pressed" : "released");
#endif
    // The key types with the layout of the device it came from.
    this->device_layout_ = this->slot_layout_(target);
    this->key_(page, usage, pressed);
    this->device_layout_ = nullptr;
  });
  input->add_on_pointer_callback([this, target](int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
    this->event_device_ = this->device_label(target);
    this->buttons_ = buttons;
    this->pointer_cb_.call(dx, dy, wheel, pan, buttons);
  });
  input->add_on_control_callback([this, target](uint16_t page, uint16_t usage, int32_t value) {
    this->event_device_ = this->device_label(target);
    this->control_cb_.call(page, usage, value);
  });
}
#endif

const hid::KeyboardLayout *BTPairing::slot_layout_(const PairingTarget *target) const {
  if (this->device_layouts_.empty() || target->pairing_slot() == 0)
    return nullptr;
  const PairingRecord *record = this->find_slot(target->pairing_slot());
  if (record == nullptr)
    return nullptr;
  for (const auto &entry : this->device_layouts_) {
    if (memcmp(entry.address, record->address, sizeof(entry.address)) == 0)
      return entry.layout;
  }
  return nullptr;
}

void BTPairing::list_devices(std::vector<hid::InputDevice> &out) const {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  for (const auto &record : this->records_) {
    bool hid = false;
    bool connected = false;
    for (const PairingTarget *target : this->hid_targets_) {
      if (target->pairing_slot() == record.slot) {
        hid = true;
        connected = target->is_device_connected();
      }
    }
    if (!hid)
      continue;
    format_mac_addr_upper(record.address, mac);
    out.push_back({std::string("bt:") + mac, record.name[0] != '\0' ? record.name : mac, connected});
  }
}

void BTPairing::set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) {
  uint8_t address[6];
  if (key.rfind("bt:", 0) != 0 || !parse_address(key.substr(3), address))
    return;
  for (auto it = this->device_layouts_.begin(); it != this->device_layouts_.end(); ++it) {
    if (memcmp(it->address, address, sizeof(address)) == 0) {
      if (layout == nullptr) {
        this->device_layouts_.erase(it);
      } else {
        it->layout = layout;
      }
      return;
    }
  }
  if (layout != nullptr) {
    DeviceLayout entry{};
    memcpy(entry.address, address, sizeof(address));
    entry.layout = layout;
    this->device_layouts_.push_back(entry);
  }
}

bool BTPairing::pair_address(const std::string &address) {
  uint8_t bda[6];
  if (!parse_address(address, bda))
    return false;
  for (size_t i = 0; i < this->scanned_.size(); i++) {
    if (memcmp(this->scanned_[i].address, bda, sizeof(bda)) == 0) {
      this->pair_with(i);
      return true;
    }
  }
  this->status_(address + " was not found by the last scan");
  return false;
}

bool BTPairing::connect(const std::string &address) {
  uint8_t bda[6];
  PairingTarget *target = parse_address(address, bda) ? this->target_of_(bda) : nullptr;
  if (target == nullptr) {
    this->status_(address + " is not paired");
    return false;
  }
  target->connect_device();
  return true;
}

bool BTPairing::disconnect(const std::string &address) {
  uint8_t bda[6];
  PairingTarget *target = parse_address(address, bda) ? this->target_of_(bda) : nullptr;
  if (target == nullptr) {
    this->status_(address + " is not paired");
    return false;
  }
  target->disconnect_device();
  return true;
}

bool BTPairing::unpair(const std::string &address) {
  uint8_t bda[6];
  return parse_address(address, bda) && this->forget_address(bda);
}

bool BTPairing::is_connected(const std::string &address) const {
  uint8_t bda[6];
  const PairingTarget *target = parse_address(address, bda) ? this->target_of_(bda) : nullptr;
  return target != nullptr && target->is_device_connected();
}

std::vector<uint8_t> BTPairing::encode_(bool fixed_size) const {
  const size_t count = fixed_size ? PREFERENCE_RECORDS : this->records_.size();
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
  std::vector<uint8_t> data(sizeof(Header) + PREFERENCE_RECORDS * sizeof(PairingRecord));
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
