#include "ble_client_hid.h"
#ifdef USE_BLE_CLIENT_HID_SELECT
#include "select/ble_hid_device_select.h"
#endif

#ifdef USE_ESP32

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "esphome/components/hid/usages.h"
#include "esphome/components/sha256/sha256.h"
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::ble_client_hid {

using hid::hid_decode_status_name;
using hid::hid_report_kind_name;
using hid::lookup_usage_kinds;
using hid::lookup_usage_name;

static const char *const TAG = "ble_client_hid";

namespace {

constexpr uint16_t UUID_GAP_SERVICE = 0x1800;
constexpr uint16_t UUID_GATT_SERVICE = 0x1801;
constexpr uint16_t UUID_HID_SERVICE = 0x1812;
constexpr uint16_t UUID_BATTERY_SERVICE = 0x180F;
constexpr uint16_t UUID_DEVICE_INFORMATION_SERVICE = 0x180A;

constexpr uint16_t UUID_DEVICE_NAME = 0x2A00;
constexpr uint16_t UUID_APPEARANCE = 0x2A01;
constexpr uint16_t UUID_PREFERRED_CONNECTION_PARAMETERS = 0x2A04;
constexpr uint16_t UUID_SERVICE_CHANGED = 0x2A05;
constexpr uint16_t UUID_SYSTEM_ID = 0x2A23;
constexpr uint16_t UUID_MODEL_NUMBER = 0x2A24;
constexpr uint16_t UUID_SERIAL_NUMBER = 0x2A25;
constexpr uint16_t UUID_FIRMWARE_REVISION = 0x2A26;
constexpr uint16_t UUID_HARDWARE_REVISION = 0x2A27;
constexpr uint16_t UUID_SOFTWARE_REVISION = 0x2A28;
constexpr uint16_t UUID_MANUFACTURER_NAME = 0x2A29;
constexpr uint16_t UUID_IEEE_CERTIFICATION = 0x2A2A;
constexpr uint16_t UUID_BATTERY_LEVEL = 0x2A19;
constexpr uint16_t UUID_PNP_ID = 0x2A50;
constexpr uint16_t UUID_HID_INFORMATION = 0x2A4A;
constexpr uint16_t UUID_HID_REPORT_MAP = 0x2A4B;
constexpr uint16_t UUID_HID_REPORT = 0x2A4D;
constexpr uint16_t UUID_HID_PROTOCOL_MODE = 0x2A4E;
constexpr uint16_t UUID_BOOT_KEYBOARD_INPUT = 0x2A22;
constexpr uint16_t UUID_BOOT_KEYBOARD_OUTPUT = 0x2A32;
constexpr uint16_t UUID_BOOT_MOUSE_INPUT = 0x2A33;
constexpr uint16_t UUID_DATABASE_HASH = 0x2B2A;
constexpr uint16_t UUID_CLIENT_SUPPORTED_FEATURES = 0x2B29;
constexpr uint16_t UUID_SERVER_SUPPORTED_FEATURES = 0x2B3A;

constexpr uint16_t UUID_CHARACTERISTIC_USER_DESCRIPTION = 0x2901;
constexpr uint16_t UUID_CCCD = 0x2902;
constexpr uint16_t UUID_CHARACTERISTIC_PRESENTATION_FORMAT = 0x2904;
constexpr uint16_t UUID_CHARACTERISTIC_AGGREGATE_FORMAT = 0x2905;
constexpr uint16_t UUID_EXTERNAL_REPORT_REFERENCE = 0x2907;
constexpr uint16_t UUID_REPORT_REFERENCE = 0x2908;

bool elapsed(uint32_t now, uint32_t start, uint32_t interval) { return static_cast<uint32_t>(now - start) >= interval; }

const char *phase_name(SetupPhase phase) {
  switch (phase) {
    case SetupPhase::DISCONNECTED:
      return "disconnected";
    case SetupPhase::WAITING_FOR_DISCOVERY:
      return "waiting_for_discovery";
    case SetupPhase::INVENTORY:
      return "inventory";
    case SetupPhase::REGISTERING:
      return "registering";
    case SetupPhase::WAITING_FOR_CCCD:
      return "waiting_for_cccd";
    case SetupPhase::READING:
      return "reading";
    case SetupPhase::BUILDING_PROFILE:
      return "building_profile";
    case SetupPhase::WRITING_PROTOCOL_MODE:
      return "writing_protocol_mode";
    case SetupPhase::READY:
      return "ready";
    case SetupPhase::DEGRADED:
      return "degraded";
    case SetupPhase::NO_HID:
      return "no_hid";
  }
  return "unknown";
}

bool is_dis_characteristic(uint16_t uuid) {
  switch (uuid) {
    case UUID_SYSTEM_ID:
    case UUID_MODEL_NUMBER:
    case UUID_SERIAL_NUMBER:
    case UUID_FIRMWARE_REVISION:
    case UUID_HARDWARE_REVISION:
    case UUID_SOFTWARE_REVISION:
    case UUID_MANUFACTURER_NAME:
    case UUID_IEEE_CERTIFICATION:
    case UUID_PNP_ID:
      return true;
    default:
      return false;
  }
}

std::string printable_text(const uint8_t *data, size_t length) {
  std::string result;
  result.reserve(length);
  for (size_t i = 0; i < length; i++)
    result.push_back(std::isprint(data[i]) != 0 ? static_cast<char>(data[i]) : '.');
  return result;
}

uint16_t uuid16_of(const espbt::ESPBTUUID &uuid) {
  const esp_bt_uuid_t raw = uuid.get_uuid();
  return raw.len == ESP_UUID_LEN_16 ? raw.uuid.uuid16 : 0;
}

}  // namespace

// -- State -------------------------------------------------------------------------------

void BLEClientHID::transition_(SetupPhase phase) {
  if (this->setup_phase_ != phase) {
    ESP_LOGV(TAG, "HID_SETUP from=%s to=%s", phase_name(this->setup_phase_), phase_name(phase));
  }
  this->setup_phase_ = phase;
  this->phase_started_ms_ = millis();
}

void BLEClientHID::mark_degraded_(const std::string &reason) {
  this->degraded_ = true;
  this->operations_failed_++;
  this->status_set_warning(reason.c_str());
  ESP_LOGW(TAG, "HID_SETUP_WARNING phase=%s reason=%s", phase_name(this->setup_phase_), reason.c_str());
}

// Everything that belongs to one connection. The report maps and the report each handle
// carries stay: they are the warm start cache, checked against the next connection's GATT
// database in select_profile_().
void BLEClientHID::reset_connection_state_() {
  this->services_.clear();
  this->hid_service_instances_.clear();
  this->read_queue_.clear();
  this->active_read_.reset();
  this->subscription_queue_.clear();
  this->active_subscription_.reset();
  this->subscription_state_.reset();
  this->hid_report_handles_.clear();
  this->battery_handles_.clear();
  this->service_changed_handles_.clear();
  this->planned_read_handles_.clear();
  this->pending_reports_.clear();
  this->pending_report_bytes_ = 0;
  this->boot_decoder_.reset();
  this->sampled_events_.clear();
  this->sampling_window_active_ = false;
  this->protocol_write_handles_.clear();
  this->keepalive_handle_ = 0;
  this->congested_ = false;
  this->rediscovery_requested_ = false;
  this->operations_succeeded_ = 0;
  this->operations_failed_ = 0;
  this->degraded_ = false;
  this->warm_start_ = false;
  this->profile_ready_ = false;
  this->gatt_profile_hash_.clear();
  this->cancel_timeout("conn_params");
}

// -- Pairing ----------------------------------------------------------------------------

namespace {

constexpr uint32_t PAIRING_CONNECT_GRACE_MS = 30000;
// GAP appearance category 0x00F: HID (keyboard, mouse, joystick, gamepad, ...).
constexpr uint16_t APPEARANCE_KEYBOARD = 0x03C1;
constexpr uint16_t APPEARANCE_HID_FIRST = 0x03C0;
constexpr uint16_t APPEARANCE_HID_LAST = 0x03FF;

std::vector<BLEClientHID *> &instances() {
  static std::vector<BLEClientHID *> list;
  return list;
}

uint64_t address_to_uint64(const uint8_t *bda) {
  uint64_t address = 0;
  for (int i = 0; i < 6; i++)
    address |= static_cast<uint64_t>(bda[i]) << ((5 - i) * 8);
  return address;
}

bool parse_mac(const std::string &text, uint8_t *bda) {
  if (text.size() != 17)
    return false;
  for (size_t i = 0; i < 6; i++) {
    if ((i > 0 && text[i * 3 - 1] != ':') || parse_hex(text.c_str() + i * 3, 2, bda + i, 1) != 2)
      return false;
  }
  return true;
}

}  // namespace

void BLEClientHID::setup() {
  instances().push_back(this);
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr && this->dynamic_)
    this->pairing_->add_target(this);
  if (this->pairing_ != nullptr) {
    this->pairing_->add_on_forget_callback([this](const bt_pairing::PairingRecord &record) {
      if (record.slot != this->slot_)
        return;
      if (this->address_set_ != nullptr)
        this->address_set_("");
      if (this->dynamic_) {
        this->schedule_address_(0, BLE_ADDR_TYPE_PUBLIC, false);
      } else if (this->parent()->state() != espbt::ClientState::IDLE) {
        this->release_all_hid();
        this->parent()->disconnect();
      }
    });
  }
#endif
}

// A slot's address changes only once its link is down; until then it does not reconnect.
void BLEClientHID::schedule_address_(uint64_t address, esp_ble_addr_type_t type, bool auto_connect) {
  this->release_all_hid();
  this->parent()->set_auto_connect(false);
  this->pending_address_ = address;
  this->pending_address_type_ = type;
  this->pending_auto_connect_ = auto_connect;
  this->pending_address_valid_ = true;
  if (this->parent()->state() != espbt::ClientState::IDLE)
    this->parent()->disconnect();
}

// The slot's device: the address global when it holds one, else the pairing database.
bool BLEClientHID::stored_address_(uint8_t *bda, esp_ble_addr_type_t *type) const {
  *type = BLE_ADDR_TYPE_PUBLIC;
  if (this->address_get_ != nullptr && parse_mac(this->address_get_(), bda))
    return true;
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr) {
    if (const auto *record = this->pairing_->find_slot(this->slot_); record != nullptr) {
      memcpy(bda, record->address, 6);
      *type = static_cast<esp_ble_addr_type_t>(record->address_type);
      return true;
    }
  }
#endif
  return false;
}

void BLEClientHID::restore_paired_() {
  if (!this->dynamic_ || this->restored_)
    return;
  uint8_t bda[6];
  esp_ble_addr_type_t type = BLE_ADDR_TYPE_PUBLIC;
  const bool from_global = this->address_get_ != nullptr && parse_mac(this->address_get_(), bda);
#ifdef USE_BT_PAIRING
  if (!from_global && this->pairing_ != nullptr && !this->pairing_->is_loaded())
    return;
#endif
  this->restored_ = true;
  if (!this->stored_address_(bda, &type) || this->parent()->get_address() != 0)
    return;
  // The advertisement the client connects on brings the current address type.
  this->parent()->set_address(address_to_uint64(bda));
  this->parent()->set_remote_addr_type(type);
  ESP_LOGV(TAG, "[%s] Paired device", this->parent()->address_str());
}

void BLEClientHID::pair(uint32_t duration_ms) {
  if (!this->dynamic_) {
    ESP_LOGV(TAG, "Configured address: connecting, which pairs it if it is not paired yet");
    this->parent()->set_auto_connect(true);
    return;
  }
  if (this->pairing_active_) {
    ESP_LOGW(TAG, "Already pairing");
    return;
  }
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr && !this->pairing_->is_loaded()) {
    ESP_LOGW(TAG, "Pairing database not loaded yet");
    return;
  }
#endif
  this->found_.clear();
  this->publish_found_();
  this->open_window_(duration_ms);
  if (this->device_select_ != nullptr) {
    ESP_LOGV(TAG, "Pairing: listing the HID devices in pairing mode, pick one");
  } else {
    ESP_LOGV(TAG, "Pairing: looking for a HID device in pairing mode");
  }
}

// The device paired so far stays stored until another one has paired.
void BLEClientHID::open_window_(uint32_t duration_ms) {
  if (this->parent()->get_address() != 0 || this->pending_address_valid_)
    this->schedule_address_(0, BLE_ADDR_TYPE_PUBLIC, false);
  this->restored_ = true;
  this->candidate_name_.clear();
  this->candidate_appearance_ = 0;
  this->candidate_selected_ = false;
  this->pairing_active_ = true;
  this->pairing_until_ = millis() + duration_ms + PAIRING_CONNECT_GRACE_MS;
}

void BLEClientHID::publish_found_() {
#ifdef USE_BLE_CLIENT_HID_SELECT
  if (this->device_select_ != nullptr)
    this->device_select_->set_devices(this->found_);
#endif
}

void BLEClientHID::pair_with(size_t index) {
  if (index < this->found_.size())
    this->pair_found_(this->found_[index]);
}

#ifdef USE_BT_PAIRING
bool BLEClientHID::is_free() const {
  if (!this->dynamic_ || this->pairing_active_)
    return false;
  return this->pending_address_valid_ ? this->pending_address_ == 0
                                      : this->ble_client::BLEClientNode::parent_->get_address() == 0;
}

bool BLEClientHID::pair_device(const bt_pairing::ScannedDevice &device) {
  return this->pair_found_({address_to_uint64(device.address), static_cast<esp_ble_addr_type_t>(device.address_type),
                            static_cast<uint16_t>(device.device_class), device.name});
}

void BLEClientHID::connect_device() {
  if (this->parent()->get_address() != 0)
    this->parent()->set_auto_connect(true);
}

void BLEClientHID::disconnect_device() {
  this->parent()->set_auto_connect(false);
  if (this->parent()->state() != espbt::ClientState::IDLE)
    this->parent()->disconnect();
}
#endif

bool BLEClientHID::pair_found_(const FoundBLEDevice &picked) {
  if (!this->dynamic_)
    return false;
  const FoundBLEDevice found = picked;
  // The window closed since, or another device was picked in it: start over with this one.
  if (!this->pairing_active_ || this->candidate_selected_)
    this->open_window_(0);
  this->candidate_name_ = found.name;
  this->candidate_appearance_ = found.appearance;
  this->candidate_address_ = found.address;
  this->candidate_selected_ = true;
  this->schedule_address_(found.address, found.address_type, true);
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  uint8_t bda[6];
  for (int i = 0; i < 6; i++)
    bda[i] = static_cast<uint8_t>(found.address >> (40 - 8 * i));
  format_mac_addr_upper(bda, mac);
  ESP_LOGV(TAG, "[%s] Pairing with '%s'", mac, found.name.c_str());
  return true;
}

#ifdef USE_ESP32_BLE_DEVICE
// Tracker: the first advertising HID device (HID service or HID appearance) no other slot has.
bool BLEClientHID::parse_device(const ble_device_base::ESPBTDevice &device) {
  if (!this->pairing_active_ || this->candidate_selected_ || this->pending_address_valid_ ||
      this->parent()->get_address() != 0)
    return false;
  bool hid = false;
  for (const auto &uuid : device.get_service_uuids())
    hid = hid || uuid16_(uuid, UUID_HID_SERVICE);
  const auto &appearance = device.get_appearance();
  if (appearance.has_value() && *appearance >= APPEARANCE_HID_FIRST && *appearance <= APPEARANCE_HID_LAST)
    hid = true;
  if (!hid)
    return false;
  const uint64_t address = device.address_uint64();
  for (auto *other : instances()) {
    if (other != this && other->parent()->get_address() == address)
      return false;
  }
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr) {
    const auto *record = this->pairing_->find_address(device.address());
    if (record != nullptr && record->slot != this->slot_ && this->pairing_->slot_in_use(record->slot))
      return false;
  }
#endif
  const StringRef name = device.get_name();
  if (this->device_select_ != nullptr) {
    // Listed only; the one picked in the select is paired.
    const std::string listed_name = name.empty() ? std::string() : std::string(name.c_str(), name.size());
    for (auto &found : this->found_) {
      if (found.address != address)
        continue;
      if (found.name.empty() && !listed_name.empty()) {
        found.name = listed_name;
        this->publish_found_();
      }
      return false;
    }
    if (this->found_.size() < MAX_FOUND) {
      this->found_.push_back(
          {address, device.get_address_type(), appearance.has_value() ? *appearance : uint16_t(0), listed_name});
      this->publish_found_();
    }
    return false;
  }
  this->candidate_name_ = name.empty() ? std::string() : std::string(name.c_str(), name.size());
  this->candidate_appearance_ = appearance.has_value() ? *appearance : 0;
  this->candidate_address_ = address;
  this->candidate_selected_ = true;
  this->parent()->set_address(address);
  this->parent()->set_remote_addr_type(device.get_address_type());
  this->parent()->set_auto_connect(true);
  ESP_LOGV(TAG, "[%s] Pairing with '%s'", this->parent()->address_str(), this->candidate_name_.c_str());
  return false;
}
#endif

void BLEClientHID::cancel_pairing() {
  if (this->pairing_active_)
    this->end_pairing_(false, "cancelled");
}

void BLEClientHID::end_pairing_(bool success, const char *reason) {
  if (!this->pairing_active_)
    return;
  // Only the candidate's own profile completes a pairing.
  if (success && (!this->candidate_selected_ || this->pending_address_valid_ ||
                  this->parent()->get_address() != this->candidate_address_))
    return;
  this->pairing_active_ = false;
  uint8_t previous[6];
  esp_ble_addr_type_t previous_type = BLE_ADDR_TYPE_PUBLIC;
  const bool has_previous = this->stored_address_(previous, &previous_type);
  const bool same_device = has_previous && address_to_uint64(previous) == this->candidate_address_;
  if (!success) {
    ESP_LOGW(TAG, "Pairing failed: %s", reason);
    // A bond the failed attempt may have left goes; the device paired before comes back.
    if (this->candidate_selected_ && !same_device)
      esp_ble_remove_bond_device(this->parent()->get_remote_bda());
    if (has_previous) {
      this->schedule_address_(address_to_uint64(previous), previous_type, true);
    } else {
      this->schedule_address_(0, BLE_ADDR_TYPE_PUBLIC, false);
    }
    return;
  }
  ESP_LOGI(TAG, "[%s] Paired with '%s'", this->parent()->address_str(), this->candidate_name_.c_str());
  // The device it replaces loses its bond.
  if (has_previous && !same_device)
    esp_ble_remove_bond_device(previous);
  if (this->address_set_ != nullptr)
    this->address_set_(this->parent()->address_str());
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr) {
    bt_pairing::PairingRecord record{};
    memcpy(record.address, this->parent()->get_remote_bda(), sizeof(record.address));
    record.transport = static_cast<uint8_t>(bt_pairing::Transport::LE);
    record.address_type = static_cast<uint8_t>(this->parent()->get_remote_addr_type());
    record.slot = this->slot_;
    record.device_class = this->candidate_appearance_;
    strncpy(record.name, this->candidate_name_.c_str(), sizeof(record.name) - 1);
    this->pairing_->store(record);
  }
#endif
}

void BLEClientHID::unpair() {
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr && this->pairing_->find_slot(this->slot_) != nullptr) {
    this->pairing_->forget_slot(this->slot_);
    return;
  }
#endif
  if (this->parent()->get_address() == 0) {
    ESP_LOGW(TAG, "No device paired");
    return;
  }
  if (esp_err_t err = esp_ble_remove_bond_device(this->parent()->get_remote_bda()); err != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Unpair failed: %s", this->parent()->address_str(), esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "[%s] Unpaired", this->parent()->address_str());
  }
  if (this->dynamic_) {
    if (this->address_set_ != nullptr)
      this->address_set_("");
    this->schedule_address_(0, BLE_ADDR_TYPE_PUBLIC, false);
  }
}

void BLEClientHID::loop() {
  if (this->dynamic_) {
    this->restore_paired_();
    if (this->pending_address_valid_ && this->parent()->state() == espbt::ClientState::IDLE) {
      this->pending_address_valid_ = false;
      this->parent()->set_address(this->pending_address_);
      if (this->pending_address_ != 0)
        this->parent()->set_remote_addr_type(this->pending_address_type_);
      this->parent()->set_auto_connect(this->pending_auto_connect_);
    }
    if (this->pairing_active_ && static_cast<int32_t>(millis() - this->pairing_until_) >= 0)
      this->end_pairing_(false, "timed out");
  }
  if (this->rediscovery_requested_) {
    this->rediscovery_requested_ = false;
    ESP_LOGV(TAG, "HID_PROFILE_INVALIDATED action=reconnect");
    this->parent()->disconnect();
  }

  switch (this->setup_phase_) {
    case SetupPhase::INVENTORY:
      if (!this->inventory_gatt_database_()) {
        this->mark_degraded_("GATT inventory failed");
        this->finalize_setup_();
      } else if (this->hid_service_instances_.empty()) {
        ESP_LOGW(TAG, "HID_READY status=NO_HID services=%u", static_cast<unsigned>(this->services_.size()));
        this->status_set_warning("No HID service");
        this->transition_(SetupPhase::NO_HID);
        this->node_state = espbt::ClientState::ESTABLISHED;
      } else {
        this->select_profile_();
        // Input reports first: a remote reports the key that is held, and does not buffer
        // the press that woke it. Reports arriving before the report map is parsed wait in
        // pending_reports_.
        this->plan_subscriptions_();
        this->transition_(SetupPhase::REGISTERING);
      }
      break;
    case SetupPhase::REGISTERING:
      this->start_next_subscription_();
      break;
    case SetupPhase::WAITING_FOR_CCCD:
      if (this->active_subscription_ != nullptr && this->subscription_state_.fallback_due(millis()))
        this->write_current_cccd_();
      break;
    case SetupPhase::READING:
      this->start_next_read_();
      break;
    case SetupPhase::BUILDING_PROFILE:
      if (!this->warm_start_)
        this->build_hid_profiles_();
      this->profile_ready_ = true;
      this->start_protocol_mode_write_();
      break;
    case SetupPhase::WRITING_PROTOCOL_MODE:
      this->start_protocol_mode_write_();
      break;
    case SetupPhase::READY:
    case SetupPhase::DEGRADED:
      this->maybe_send_keepalive_();
      break;
    default:
      break;
  }

  this->handle_phase_timeout_();
  if (!this->profile_ready_)
    return;
  this->update_sampling_window_();
  const uint32_t processing_started = micros();
  size_t processed = 0;
  while (!this->pending_reports_.empty() && processed < MAX_REPORTS_PER_LOOP &&
         static_cast<uint32_t>(micros() - processing_started) < MAX_PROCESSING_TIME_US) {
    PendingHIDReport report = std::move(this->pending_reports_.front());
    this->pending_reports_.pop_front();
    this->pending_report_bytes_ -= report.payload.size();
    this->process_input_report_(report);
    processed++;
  }
  this->update_sampling_window_();
}

void BLEClientHID::handle_phase_timeout_() {
  if (this->setup_phase_ == SetupPhase::DISCONNECTED || this->setup_phase_ == SetupPhase::WAITING_FOR_DISCOVERY ||
      this->setup_phase_ == SetupPhase::READY || this->setup_phase_ == SetupPhase::DEGRADED ||
      this->setup_phase_ == SetupPhase::NO_HID || !elapsed(millis(), this->phase_started_ms_, PHASE_TIMEOUT_MS))
    return;
  const std::string reason = std::string("timeout in ") + phase_name(this->setup_phase_);
  this->mark_degraded_(reason);
  if (this->setup_phase_ == SetupPhase::REGISTERING || this->setup_phase_ == SetupPhase::WAITING_FOR_CCCD) {
    this->active_subscription_.reset();
    this->subscription_state_.reset();
    this->subscription_queue_.clear();
    this->begin_reads_();
  } else if (this->setup_phase_ == SetupPhase::READING) {
    this->active_read_.reset();
    this->read_queue_.clear();
    this->transition_(SetupPhase::BUILDING_PROFILE);
  } else {
    this->finalize_setup_();
  }
}

uint8_t BLEClientHID::get_device_kinds() const {
  if (!this->hid_connected_)
    return 0;
  uint8_t kinds = 0;
  for (const auto &context : this->hid_services_) {
    if (context.report_map != nullptr)
      kinds |= context.report_map->device_kinds();
  }
  return kinds;
}

void BLEClientHID::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Client HID:");
  ESP_LOGCONFIG(TAG, "  MAC address: %s", this->dynamic_ ? "set by pairing" : this->parent()->address_str());
  ESP_LOGCONFIG(TAG, "  Discovery mode: %s",
                this->discovery_mode_ == DiscoveryMode::FORENSIC ? "forensic" : "standard");
  const char *protocol = this->protocol_mode_policy_ == ProtocolModePolicy::REPORT ? "report"
                         : this->protocol_mode_policy_ == ProtocolModePolicy::BOOT ? "boot"
                                                                                   : "unchanged";
  ESP_LOGCONFIG(TAG, "  Protocol mode: %s", protocol);
  if (this->event_sampling_interval_us_ == 0) {
    ESP_LOGCONFIG(TAG, "  Event coalescing: disabled");
  } else {
    ESP_LOGCONFIG(TAG, "  Event coalescing: %" PRIu32 " us", this->event_sampling_interval_us_);
  }
  ESP_LOGCONFIG(TAG, "  Home Assistant events: %s", YESNO(this->publish_to_ha_));
  if (this->keepalive_interval_ms_ != 0) {
    ESP_LOGCONFIG(TAG, "  Keepalive: every %" PRIu32 " ms, %s", this->keepalive_interval_ms_,
                  this->keepalive_enabled_ ? "enabled" : "disabled");
  }
  if (this->has_conn_latency_)
    ESP_LOGCONFIG(TAG, "  Connection latency: %u", this->conn_latency_);
  ESP_LOGCONFIG(TAG, "  Report maps cached: %s", YESNO(this->cache_valid_));
  ESP_LOGCONFIG(TAG, "  HID Usage Tables: %s", hid::HID_USAGE_TABLE_VERSION);
}

// -- Helpers -----------------------------------------------------------------------------

bool BLEClientHID::uuid16_(const espbt::ESPBTUUID &uuid, uint16_t expected) {
  const esp_bt_uuid_t raw = uuid.get_uuid();
  return raw.len == ESP_UUID_LEN_16 && raw.uuid.uuid16 == expected;
}

std::string BLEClientHID::uuid_string_(const espbt::ESPBTUUID &uuid) {
  char buffer[espbt::UUID_STR_LEN];
  return uuid.as_128bit().to_str(buffer);
}

std::string BLEClientHID::format_value_(const uint8_t *data, size_t length) {
  if (data == nullptr && length != 0)
    return "<null>";
  if (length == 0)
    return "<empty>";
  return format_hex_pretty(data, length);
}

std::string BLEClientHID::sha256_(const uint8_t *data, size_t length) {
  sha256::SHA256 hasher;
  hasher.init();
  hasher.add(data, length);
  hasher.calculate();
  char hex[65];
  hasher.get_hex(hex);
  return hex;
}

const GattCharacteristicInfo *BLEClientHID::find_characteristic_(uint16_t handle) const {
  for (const auto &service : this->services_) {
    for (const auto &characteristic : service.characteristics) {
      if (characteristic.handle == handle)
        return &characteristic;
    }
  }
  return nullptr;
}

const GattDescriptorInfo *BLEClientHID::find_descriptor_(const GattCharacteristicInfo &characteristic,
                                                         uint16_t uuid) const {
  for (const auto &descriptor : characteristic.descriptors) {
    if (uuid16_(descriptor.uuid, uuid))
      return &descriptor;
  }
  return nullptr;
}

HIDServiceContext *BLEClientHID::find_hid_context_(uint16_t service_instance) {
  for (auto &context : this->hid_services_) {
    if (context.service_instance == service_instance)
      return &context;
  }
  return nullptr;
}

const HIDServiceContext *BLEClientHID::find_hid_context_(uint16_t service_instance) const {
  for (const auto &context : this->hid_services_) {
    if (context.service_instance == service_instance)
      return &context;
  }
  return nullptr;
}

// -- Inventory ---------------------------------------------------------------------------

bool BLEClientHID::inventory_gatt_database_() {
  this->services_.clear();
  this->hid_service_instances_.clear();
  size_t characteristic_total = 0;
  size_t descriptor_total = 0;
  size_t included_service_total = 0;
  // The hash identifies the database, handles included: a warm start reuses report maps
  // and report references by handle.
  sha256::SHA256 normalized;
  normalized.init();
  const auto gattc_if = this->parent()->get_gattc_if();
  const auto conn_id = this->parent()->get_conn_id();

  for (uint16_t service_offset = 0; service_offset < MAX_SERVICES; service_offset++) {
    esp_gattc_service_elem_t result{};
    uint16_t count = 1;
    const esp_gatt_status_t status =
        esp_ble_gattc_get_service(gattc_if, conn_id, nullptr, &result, &count, service_offset);
    if (status == ESP_GATT_INVALID_OFFSET || status == ESP_GATT_NOT_FOUND)
      break;
    if (status != ESP_GATT_OK || count == 0) {
      ESP_LOGV(TAG, "GATT_INVENTORY_ERROR object=service offset=%u status=%d", service_offset, status);
      return false;
    }
    GattServiceInfo service;
    service.uuid = espbt::ESPBTUUID::from_uuid(result.uuid);
    service.instance = service_offset;
    service.start_handle = result.start_handle;
    service.end_handle = result.end_handle;
    service.primary = result.is_primary;
    normalized.add("S:" + uuid_string_(service.uuid) + (service.primary ? ":P:" : ":S:") +
                   std::to_string(service.start_handle) + "-" + std::to_string(service.end_handle) + ";");
    ESP_LOGV(TAG, "GATT_SERVICE instance=%u uuid=%s primary=%s start=%u end=%u", service.instance,
             uuid_string_(service.uuid).c_str(), service.primary ? "true" : "false", service.start_handle,
             service.end_handle);

    uint16_t include_count = 0;
    if (esp_ble_gattc_get_attr_count(gattc_if, conn_id, ESP_GATT_DB_INCLUDED_SERVICE, service.start_handle,
                                     service.end_handle, 0, &include_count) == ESP_GATT_OK &&
        include_count != 0) {
      const size_t remaining = MAX_INCLUDED_SERVICES - included_service_total;
      if (include_count > remaining) {
        this->mark_degraded_("included service inventory limit exceeded");
        include_count = static_cast<uint16_t>(remaining);
      }
      std::vector<esp_gattc_incl_svc_elem_t> includes(include_count);
      uint16_t requested = include_count;
      const esp_gatt_status_t include_status =
          requested == 0 ? ESP_GATT_OK
                         : esp_ble_gattc_get_include_service(gattc_if, conn_id, service.start_handle,
                                                             service.end_handle, nullptr, includes.data(), &requested);
      if (include_status == ESP_GATT_OK) {
        for (uint16_t i = 0; i < requested; i++) {
          GattIncludedServiceInfo include;
          include.uuid = espbt::ESPBTUUID::from_uuid(includes[i].uuid);
          include.declaration_handle = includes[i].handle;
          include.start_handle = includes[i].incl_srvc_s_handle;
          include.end_handle = includes[i].incl_srvc_e_handle;
          normalized.add("I:" + uuid_string_(include.uuid) + ":" + std::to_string(include.declaration_handle) + ";");
          ESP_LOGV(TAG, "GATT_INCLUDE service_instance=%u uuid=%s handle=%u start=%u end=%u", service.instance,
                   uuid_string_(include.uuid).c_str(), include.declaration_handle, include.start_handle,
                   include.end_handle);
          service.included_services.push_back(std::move(include));
          included_service_total++;
        }
      } else {
        this->mark_degraded_("included service inventory failed");
      }
    }

    for (uint16_t char_offset = 0; characteristic_total < MAX_CHARACTERISTICS; char_offset++) {
      esp_gattc_char_elem_t char_result{};
      uint16_t char_count = 1;
      const esp_gatt_status_t char_status = esp_ble_gattc_get_all_char(
          gattc_if, conn_id, service.start_handle, service.end_handle, &char_result, &char_count, char_offset);
      if (char_status == ESP_GATT_INVALID_OFFSET || char_status == ESP_GATT_NOT_FOUND)
        break;
      if (char_status != ESP_GATT_OK || char_count == 0) {
        this->mark_degraded_("characteristic inventory failed");
        break;
      }
      GattCharacteristicInfo characteristic;
      characteristic.uuid = espbt::ESPBTUUID::from_uuid(char_result.uuid);
      characteristic.handle = char_result.char_handle;
      characteristic.properties = char_result.properties;
      characteristic.service_instance = service.instance;
      normalized.add("C:" + uuid_string_(characteristic.uuid) + ":" + std::to_string(characteristic.properties) + ":" +
                     std::to_string(characteristic.handle) + ";");
      ESP_LOGV(TAG, "GATT_CHARACTERISTIC service_instance=%u uuid=%s handle=%u properties=0x%02X", service.instance,
               uuid_string_(characteristic.uuid).c_str(), characteristic.handle, characteristic.properties);
      characteristic_total++;

      for (uint16_t descriptor_offset = 0; descriptor_total < MAX_DESCRIPTORS; descriptor_offset++) {
        esp_gattc_descr_elem_t descriptor_result{};
        uint16_t descriptor_count = 1;
        const esp_gatt_status_t descriptor_status = esp_ble_gattc_get_all_descr(
            gattc_if, conn_id, characteristic.handle, &descriptor_result, &descriptor_count, descriptor_offset);
        if (descriptor_status == ESP_GATT_INVALID_OFFSET || descriptor_status == ESP_GATT_NOT_FOUND)
          break;
        if (descriptor_status != ESP_GATT_OK || descriptor_count == 0) {
          this->mark_degraded_("descriptor inventory failed");
          break;
        }
        GattDescriptorInfo descriptor;
        descriptor.uuid = espbt::ESPBTUUID::from_uuid(descriptor_result.uuid);
        descriptor.handle = descriptor_result.handle;
        normalized.add("D:" + uuid_string_(descriptor.uuid) + ":" + std::to_string(descriptor.handle) + ";");
        ESP_LOGV(TAG, "GATT_DESCRIPTOR service_instance=%u characteristic_handle=%u uuid=%s handle=%u",
                 service.instance, characteristic.handle, uuid_string_(descriptor.uuid).c_str(), descriptor.handle);
        characteristic.descriptors.push_back(std::move(descriptor));
        descriptor_total++;
      }
      if (descriptor_total >= MAX_DESCRIPTORS)
        this->mark_degraded_("descriptor inventory limit exceeded");
      service.characteristics.push_back(std::move(characteristic));
    }
    if (characteristic_total >= MAX_CHARACTERISTICS)
      this->mark_degraded_("characteristic inventory limit exceeded");

    if (uuid16_(service.uuid, UUID_HID_SERVICE))
      this->hid_service_instances_.push_back(service.instance);
    this->services_.push_back(std::move(service));
  }
  if (this->services_.size() >= MAX_SERVICES)
    this->mark_degraded_("service inventory limit exceeded");
  normalized.calculate();
  char hex[65];
  normalized.get_hex(hex);
  this->gatt_profile_hash_ = hex;
  ESP_LOGV(TAG, "GATT_PROFILE services=%u included_services=%u characteristics=%u descriptors=%u sha256=%s",
           static_cast<unsigned>(this->services_.size()), static_cast<unsigned>(included_service_total),
           static_cast<unsigned>(characteristic_total), static_cast<unsigned>(descriptor_total),
           this->gatt_profile_hash_.c_str());
  return !this->services_.empty();
}

// Reuse the report maps of the last connection if the database is the same, otherwise start
// over with an empty context per HID service.
void BLEClientHID::select_profile_() {
  this->warm_start_ = this->cache_valid_ && this->cached_profile_hash_ == this->gatt_profile_hash_ &&
                      this->hid_services_.size() == this->hid_service_instances_.size();
  if (this->warm_start_) {
    for (auto &context : this->hid_services_) {
      if (context.report_map != nullptr)
        context.report_map->reset_runtime_state();
    }
    this->profile_ready_ = true;
    ESP_LOGV(TAG, "HID_PROFILE source=cache sha256=%s", this->gatt_profile_hash_.c_str());
    return;
  }
  if (this->cache_valid_)
    ESP_LOGV(TAG, "HID_PROFILE source=rebuild reason=gatt_database_changed");
  this->cache_valid_ = false;
  this->hid_services_.clear();
  this->handle_report_source_.clear();
  this->preferred_conn_params_valid_ = false;
  this->preferred_conn_params_ = {};
  for (const uint16_t instance : this->hid_service_instances_) {
    HIDServiceContext context;
    context.service_instance = instance;
    this->hid_services_.push_back(std::move(context));
  }
}

// -- Reads -------------------------------------------------------------------------------

bool BLEClientHID::add_read_(const GattServiceInfo &service, const GattCharacteristicInfo &characteristic,
                             const GattDescriptorInfo *descriptor, ReadPurpose purpose, bool required) {
  const uint16_t handle = descriptor == nullptr ? characteristic.handle : descriptor->handle;
  if (this->planned_read_handles_.count(handle) != 0)
    return true;
  if (this->read_queue_.size() >= MAX_READ_OPERATIONS) {
    this->mark_degraded_("read operation limit exceeded");
    return false;
  }
  GattReadOperation operation;
  operation.handle = handle;
  operation.characteristic_handle = characteristic.handle;
  operation.service_instance = service.instance;
  operation.purpose = purpose;
  operation.descriptor = descriptor != nullptr;
  operation.required = required;
  operation.service_uuid = service.uuid;
  operation.characteristic_uuid = characteristic.uuid;
  operation.attribute_uuid = descriptor == nullptr ? characteristic.uuid : descriptor->uuid;
  this->read_queue_.push_back(std::move(operation));
  this->planned_read_handles_.insert(handle);
  return true;
}

void BLEClientHID::plan_reads_() {
  this->read_queue_.clear();
  this->planned_read_handles_.clear();
  for (const auto &service : this->services_) {
    for (const auto &characteristic : service.characteristics) {
      const uint16_t uuid = uuid16_of(characteristic.uuid);
      const bool battery = uuid16_(service.uuid, UUID_BATTERY_SERVICE) && uuid == UUID_BATTERY_LEVEL;
      // A warm start knows everything but the battery level.
      if (this->warm_start_) {
        if (battery && (characteristic.properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0)
          this->add_read_(service, characteristic, nullptr, ReadPurpose::BATTERY_LEVEL, false);
        continue;
      }
      ReadPurpose purpose = ReadPurpose::GENERIC_STANDARD;
      bool standard = false;
      bool required = false;
      if (uuid16_(service.uuid, UUID_GAP_SERVICE)) {
        if (uuid == UUID_DEVICE_NAME) {
          purpose = ReadPurpose::DEVICE_NAME;
        } else if (uuid == UUID_APPEARANCE) {
          purpose = ReadPurpose::APPEARANCE;
        } else if (uuid == UUID_PREFERRED_CONNECTION_PARAMETERS) {
          purpose = ReadPurpose::PREFERRED_CONNECTION_PARAMETERS;
        }
        standard = uuid == UUID_DEVICE_NAME || uuid == UUID_APPEARANCE || uuid == UUID_PREFERRED_CONNECTION_PARAMETERS;
      } else if (uuid16_(service.uuid, UUID_GATT_SERVICE)) {
        standard = uuid == UUID_SERVICE_CHANGED || uuid == UUID_DATABASE_HASH ||
                   uuid == UUID_CLIENT_SUPPORTED_FEATURES || uuid == UUID_SERVER_SUPPORTED_FEATURES;
      } else if (uuid16_(service.uuid, UUID_DEVICE_INFORMATION_SERVICE) && is_dis_characteristic(uuid)) {
        standard = true;
        if (uuid == UUID_PNP_ID) {
          purpose = ReadPurpose::PNP_ID;
        } else if (uuid == UUID_MANUFACTURER_NAME) {
          purpose = ReadPurpose::MANUFACTURER;
        } else if (uuid == UUID_SERIAL_NUMBER) {
          purpose = ReadPurpose::SERIAL_NUMBER;
        } else if (uuid == UUID_FIRMWARE_REVISION) {
          purpose = ReadPurpose::FIRMWARE_REVISION;
        }
      } else if (battery) {
        standard = true;
        purpose = ReadPurpose::BATTERY_LEVEL;
      } else if (uuid16_(service.uuid, UUID_HID_SERVICE)) {
        standard = uuid == UUID_HID_INFORMATION || uuid == UUID_HID_REPORT_MAP || uuid == UUID_HID_PROTOCOL_MODE ||
                   uuid == UUID_HID_REPORT || uuid == UUID_BOOT_KEYBOARD_INPUT || uuid == UUID_BOOT_KEYBOARD_OUTPUT ||
                   uuid == UUID_BOOT_MOUSE_INPUT;
        if (uuid == UUID_HID_INFORMATION) {
          purpose = ReadPurpose::HID_INFORMATION;
        } else if (uuid == UUID_HID_REPORT_MAP) {
          purpose = ReadPurpose::HID_REPORT_MAP;
          required = true;
        } else if (uuid == UUID_HID_PROTOCOL_MODE) {
          purpose = ReadPurpose::HID_PROTOCOL_MODE;
        } else {
          purpose = ReadPurpose::HID_REPORT_VALUE;
        }
      }
      if ((characteristic.properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0 &&
          (standard || this->discovery_mode_ == DiscoveryMode::FORENSIC))
        this->add_read_(service, characteristic, nullptr, standard ? purpose : ReadPurpose::GENERIC_FORENSIC, required);

      for (const auto &descriptor : characteristic.descriptors) {
        ReadPurpose descriptor_purpose = ReadPurpose::GENERIC_STANDARD;
        bool standard_descriptor = false;
        if (uuid16_(descriptor.uuid, UUID_REPORT_REFERENCE)) {
          standard_descriptor = true;
          descriptor_purpose = ReadPurpose::HID_REPORT_REFERENCE;
        } else if (uuid16_(descriptor.uuid, UUID_EXTERNAL_REPORT_REFERENCE)) {
          standard_descriptor = true;
          descriptor_purpose = ReadPurpose::HID_EXTERNAL_REPORT_REFERENCE;
        } else if (uuid16_(descriptor.uuid, UUID_CHARACTERISTIC_USER_DESCRIPTION) ||
                   uuid16_(descriptor.uuid, UUID_CHARACTERISTIC_PRESENTATION_FORMAT) ||
                   uuid16_(descriptor.uuid, UUID_CHARACTERISTIC_AGGREGATE_FORMAT)) {
          standard_descriptor = true;
        }
        if (standard_descriptor || this->discovery_mode_ == DiscoveryMode::FORENSIC)
          this->add_read_(service, characteristic, &descriptor,
                          standard_descriptor ? descriptor_purpose : ReadPurpose::GENERIC_FORENSIC, false);
      }
    }
  }
  ESP_LOGV(TAG, "GATT_READ_PLAN operations=%u mode=%s start=%s", static_cast<unsigned>(this->read_queue_.size()),
           this->discovery_mode_ == DiscoveryMode::FORENSIC ? "forensic" : "standard",
           this->warm_start_ ? "warm" : "cold");
}

void BLEClientHID::begin_reads_() {
  this->plan_reads_();
  this->transition_(SetupPhase::READING);
}

void BLEClientHID::start_next_read_() {
  if (this->active_read_ != nullptr || this->congested_)
    return;
  if (this->read_queue_.empty()) {
    this->transition_(SetupPhase::BUILDING_PROFILE);
    return;
  }
  this->active_read_ = std::make_unique<GattReadOperation>(std::move(this->read_queue_.front()));
  this->read_queue_.pop_front();
  const esp_err_t status =
      this->active_read_->descriptor
          ? esp_ble_gattc_read_char_descr(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                          this->active_read_->handle, this->read_auth_())
          : esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                    this->active_read_->handle, this->read_auth_());
  if (status != ESP_OK) {
    const bool required = this->active_read_->required;
    ESP_LOGW(TAG, "GATT_READ handle=%u status=schedule_failed code=%d", this->active_read_->handle, status);
    this->active_read_.reset();
    if (required) {
      this->mark_degraded_("required GATT read could not be scheduled");
    } else {
      this->operations_failed_++;
    }
  }
}

void BLEClientHID::finish_active_read_(esp_gatt_status_t status, const uint8_t *value, size_t length) {
  if (this->active_read_ == nullptr) {
    ESP_LOGV(TAG, "GATT_READ status=unexpected_callback");
    return;
  }
  GattReadOperation operation = std::move(*this->active_read_);
  this->active_read_.reset();
  if (status != ESP_GATT_OK) {
    ESP_LOGV(TAG, "GATT_READ handle=%u uuid=%s status=failed code=%d", operation.handle,
             uuid_string_(operation.attribute_uuid).c_str(), status);
    if (operation.required) {
      this->mark_degraded_("required GATT read failed");
    } else if (operation.purpose != ReadPurpose::KEEPALIVE) {
      this->operations_failed_++;
    }
    return;
  }
  if (length > MAX_ATTRIBUTE_VALUE_BYTES) {
    ESP_LOGV(TAG, "GATT_READ handle=%u status=value_too_large length=%u limit=%u", operation.handle,
             static_cast<unsigned>(length), static_cast<unsigned>(MAX_ATTRIBUTE_VALUE_BYTES));
    if (operation.required) {
      this->mark_degraded_("required GATT value exceeded limit");
    } else {
      this->operations_failed_++;
    }
    return;
  }
  if (operation.purpose != ReadPurpose::KEEPALIVE)
    this->operations_succeeded_++;
  this->process_read_value_(operation, value, length);
}

void BLEClientHID::process_read_value_(const GattReadOperation &operation, const uint8_t *value, size_t length) {
  ESP_LOGV(TAG, "GATT_VALUE service_instance=%u characteristic_handle=%u handle=%u uuid=%s len=%u data=%s",
           operation.service_instance, operation.characteristic_handle, operation.handle,
           uuid_string_(operation.attribute_uuid).c_str(), static_cast<unsigned>(length),
           format_value_(value, length).c_str());
  if (value == nullptr && length != 0) {
    if (operation.required)
      this->mark_degraded_("required GATT read returned null data");
    return;
  }
  switch (operation.purpose) {
    case ReadPurpose::DEVICE_NAME:
      ESP_LOGV(TAG, "HID_DEVICE field=device_name len=%u value=%s", static_cast<unsigned>(length),
               printable_text(value, length).c_str());
      break;
    case ReadPurpose::MANUFACTURER:
      ESP_LOGV(TAG, "HID_DEVICE field=manufacturer len=%u value=%s", static_cast<unsigned>(length),
               printable_text(value, length).c_str());
      break;
    case ReadPurpose::SERIAL_NUMBER:
      ESP_LOGV(TAG, "HID_DEVICE field=serial_number len=%u value=%s privacy=sensitive", static_cast<unsigned>(length),
               printable_text(value, length).c_str());
      break;
    case ReadPurpose::FIRMWARE_REVISION:
      this->set_firmware(printable_text(value, length).c_str());
      break;
    case ReadPurpose::APPEARANCE:
      if (length == 2) {
        ESP_LOGV(TAG, "HID_DEVICE field=appearance value=%u", value[0] | (static_cast<uint16_t>(value[1]) << 8U));
      } else {
        this->mark_degraded_("Appearance has invalid length");
      }
      break;
    case ReadPurpose::PREFERRED_CONNECTION_PARAMETERS:
      if (length == 8) {
        this->preferred_conn_params_.min_int = value[0] | (static_cast<uint16_t>(value[1]) << 8U);
        this->preferred_conn_params_.max_int = value[2] | (static_cast<uint16_t>(value[3]) << 8U);
        this->preferred_conn_params_.latency = value[4] | (static_cast<uint16_t>(value[5]) << 8U);
        this->preferred_conn_params_.timeout = value[6] | (static_cast<uint16_t>(value[7]) << 8U);
        this->preferred_conn_params_valid_ = this->validate_preferred_conn_params_();
      } else {
        this->mark_degraded_("Preferred Connection Parameters have invalid length");
      }
      break;
    case ReadPurpose::BATTERY_LEVEL:
      this->battery_handles_.insert(operation.characteristic_handle);
      if (length >= 1) {
        ESP_LOGV(TAG, "HID_BATTERY source=read value=%u", value[0]);
        this->set_battery(static_cast<int8_t>(std::min<uint8_t>(value[0], 100)), this->get_details().battery_state);
#ifdef USE_SENSOR
        if (this->battery_sensor_ != nullptr)
          this->battery_sensor_->publish_state(value[0]);
#endif
      } else {
        this->mark_degraded_("Battery Level is empty");
      }
      break;
    case ReadPurpose::KEEPALIVE:
      if (this->battery_handles_.count(operation.characteristic_handle) != 0 && length >= 1)
        this->set_battery(static_cast<int8_t>(std::min<uint8_t>(value[0], 100)), this->get_details().battery_state);
#ifdef USE_SENSOR
      // The keepalive reads the battery level where there is one.
      if (this->battery_handles_.count(operation.characteristic_handle) != 0 && length >= 1 &&
          this->battery_sensor_ != nullptr)
        this->battery_sensor_->publish_state(value[0]);
#endif
      break;
    case ReadPurpose::PNP_ID:
      if (length >= 7) {
        const uint16_t vendor_id = value[1] | (static_cast<uint16_t>(value[2]) << 8U);
        const uint16_t product_id = value[3] | (static_cast<uint16_t>(value[4]) << 8U);
        const uint16_t version = value[5] | (static_cast<uint16_t>(value[6]) << 8U);
        ESP_LOGV(TAG, "HID_DEVICE field=pnp_id source=%u vendor_id=0x%04X product_id=0x%04X version=0x%04X", value[0],
                 vendor_id, product_id, version);
        this->pnp_vid_ = vendor_id;
        this->pnp_pid_ = product_id;
        this->pnp_version_ = version;
      } else {
        this->mark_degraded_("PnP ID has invalid length");
      }
      break;
    case ReadPurpose::HID_INFORMATION: {
      const HIDServiceContext *context = this->find_hid_context_(operation.service_instance);
      if (context != nullptr && length == 4) {
        ESP_LOGV(TAG, "HID_INFO service_instance=%u bcd_hid=0x%02X%02X country=%u flags=0x%02X",
                 operation.service_instance, value[1], value[0], value[2], value[3]);
      } else {
        this->mark_degraded_("HID Information has invalid length");
      }
      break;
    }
    case ReadPurpose::HID_PROTOCOL_MODE: {
      HIDServiceContext *context = this->find_hid_context_(operation.service_instance);
      if (context != nullptr && length == 1) {
        context->protocol_mode = value[0];
        context->protocol_mode_handle = operation.characteristic_handle;
        ESP_LOGV(TAG, "HID_PROTOCOL service_instance=%u mode=%s raw=%u", operation.service_instance,
                 value[0] == 0   ? "boot"
                 : value[0] == 1 ? "report"
                                 : "unknown",
                 value[0]);
      } else {
        this->mark_degraded_("Protocol Mode has invalid length");
      }
      break;
    }
    case ReadPurpose::HID_REPORT_MAP: {
      HIDServiceContext *context = this->find_hid_context_(operation.service_instance);
      if (context == nullptr || length == 0) {
        this->mark_degraded_("HID Report Map is empty");
        break;
      }
      context->report_map_hash = sha256_(value, length);
      context->report_map.reset(HIDReportMap::parse_report_map_data(value, length));
      ESP_LOGV(TAG, "HID_REPORT_MAP service_instance=%u len=%u sha256=%s", operation.service_instance,
               static_cast<unsigned>(length), context->report_map_hash.c_str());
      ESP_LOGV(TAG, "HID_REPORT_MAP data=%s", format_value_(value, length).c_str());
      HIDReportMap::esp_logd_report_map(value, length);
      if (context->report_map == nullptr)
        this->mark_degraded_("HID Report Map could not be parsed");
      break;
    }
    case ReadPurpose::HID_REPORT_REFERENCE: {
      HIDReportSource &source = this->handle_report_source_[operation.characteristic_handle];
      source.service_instance = operation.service_instance;
      source.characteristic_handle = operation.characteristic_handle;
      source.characteristic_uuid = UUID_HID_REPORT;
      if (length == 2) {
        source.report_id = value[0];
        source.has_report_id = true;
        source.report_type = value[1];
        source.has_report_type = true;
        ESP_LOGV(TAG, "HID_REPORT_REFERENCE service_instance=%u handle=%u report_id=%u report_type=%u",
                 operation.service_instance, operation.characteristic_handle, value[0], value[1]);
        if (value[1] < 1 || value[1] > 3)
          this->mark_degraded_("Report Reference has unknown Report Type");
      } else {
        this->mark_degraded_("Report Reference has invalid length");
      }
      break;
    }
    case ReadPurpose::HID_EXTERNAL_REPORT_REFERENCE:
      if (length == 2) {
        ESP_LOGV(TAG, "HID_EXTERNAL_REPORT_REFERENCE service_instance=%u uuid=0x%04X", operation.service_instance,
                 value[0] | (static_cast<uint16_t>(value[1]) << 8U));
      } else {
        this->mark_degraded_("External Report Reference has unsupported length");
      }
      break;
    default:
      break;
  }
}

// -- Profile -----------------------------------------------------------------------------

void BLEClientHID::log_hid_schema_(const HIDServiceContext &context) const {
  if (context.report_map == nullptr)
    return;
  for (const auto &diagnostic : context.report_map->diagnostics()) {
    if (diagnostic.error) {
      ESP_LOGV(TAG, "HID_SCHEMA_WARNING service_instance=%u offset=%u message=%s", context.service_instance,
               static_cast<unsigned>(diagnostic.offset), diagnostic.message.c_str());
    } else {
      ESP_LOGV(TAG, "HID_SCHEMA_DIAGNOSTIC service_instance=%u offset=%u message=%s", context.service_instance,
               static_cast<unsigned>(diagnostic.offset), diagnostic.message.c_str());
    }
  }
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  for (const auto &collection : context.report_map->collections()) {
    const char *usage_name = lookup_usage_name(collection.usage.page, collection.usage.usage);
    ESP_LOGV(TAG,
             "HID_COLLECTION service_instance=%u collection_id=%u parent=%s type=%u usage_page=%u usage_id=%u "
             "usage=%s aliases=%u",
             context.service_instance, collection.id,
             collection.parent == HIDCollectionSchema::NO_PARENT ? "-" : std::to_string(collection.parent).c_str(),
             collection.type, collection.usage.page, collection.usage.usage,
             usage_name == nullptr ? "unknown" : usage_name, static_cast<unsigned>(collection.aliases.size()));
  }
  for (const auto &report : context.report_map->reports()) {
    ESP_LOGV(TAG, "HID_REPORT_SCHEMA service_instance=%u kind=%s report_id=%u bits=%u bytes=%u fields=%u",
             context.service_instance, hid_report_kind_name(report.kind), report.report_id,
             static_cast<unsigned>(report.bit_size), static_cast<unsigned>(report.byte_size()),
             static_cast<unsigned>(report.field_ids.size()));
  }
  for (const auto &field : context.report_map->fields()) {
    const HIDUsage first_usage = field.usage_at(0, false);
    const char *usage_name = lookup_usage_name(first_usage.page, first_usage.usage);
    const char *usage_kinds = lookup_usage_kinds(first_usage.page, first_usage.usage);
    ESP_LOGV(TAG,
             "HID_FIELD service_instance=%u field_id=%u kind=%s report_id=%u collection_id=%s offset=%u size=%u "
             "count=%u flags=0x%03X logical_min=%lld logical_max=%lld physical_min=%lld physical_max=%lld "
             "unit=0x%08X unit_exp=%d usage_page=%u usage_id=%u usage=%s usage_kinds=%s aliases=%u strings=%u "
             "designators=%u",
             context.service_instance, field.id, hid_report_kind_name(field.kind), field.report_id,
             field.collection_id == HIDCollectionSchema::NO_PARENT ? "-" : std::to_string(field.collection_id).c_str(),
             static_cast<unsigned>(field.bit_offset), static_cast<unsigned>(field.report_size),
             static_cast<unsigned>(field.report_count), field.flags, static_cast<long long>(field.logical.minimum),
             static_cast<long long>(field.logical.maximum), static_cast<long long>(field.physical.minimum),
             static_cast<long long>(field.physical.maximum), static_cast<unsigned>(field.unit), field.unit_exponent,
             first_usage.page, first_usage.usage, usage_name == nullptr ? "unknown" : usage_name,
             usage_kinds == nullptr ? "-" : usage_kinds, static_cast<unsigned>(field.alternative_usages.size()),
             static_cast<unsigned>(field.string_indices.size()),
             static_cast<unsigned>(field.designator_indices.size()));
  }
#endif
}

void BLEClientHID::build_hid_profiles_() {
  for (const auto &service : this->services_) {
    if (!uuid16_(service.uuid, UUID_HID_SERVICE))
      continue;
    HIDServiceContext *context = this->find_hid_context_(service.instance);
    for (const auto &characteristic : service.characteristics) {
      const uint16_t uuid = uuid16_of(characteristic.uuid);
      if (uuid == UUID_HID_REPORT || uuid == UUID_BOOT_KEYBOARD_INPUT || uuid == UUID_BOOT_MOUSE_INPUT) {
        HIDReportSource &source = this->handle_report_source_[characteristic.handle];
        source.service_instance = service.instance;
        source.characteristic_handle = characteristic.handle;
        source.characteristic_uuid = uuid;
        if (uuid == UUID_BOOT_KEYBOARD_INPUT || uuid == UUID_BOOT_MOUSE_INPUT) {
          source.report_type = static_cast<uint8_t>(HIDReportKind::INPUT);
          source.has_report_type = true;
        } else if (!source.has_report_id) {
          ESP_LOGV(TAG, "HID_SCHEMA_WARNING service_instance=%u handle=%u reason=missing_report_reference",
                   service.instance, characteristic.handle);
          this->degraded_ = true;
        }
      }
      if (uuid == UUID_HID_PROTOCOL_MODE && context != nullptr)
        context->protocol_mode_handle = characteristic.handle;
    }
    if (context == nullptr || context->report_map == nullptr)
      continue;
    this->log_hid_schema_(*context);
    for (const auto &entry : this->handle_report_source_) {
      const HIDReportSource &source = entry.second;
      if (source.service_instance != service.instance || !source.has_report_id || !source.has_report_type)
        continue;
      const auto kind = source.report_type == 1   ? HIDReportKind::INPUT
                        : source.report_type == 2 ? HIDReportKind::OUTPUT
                                                  : HIDReportKind::FEATURE;
      if (source.report_type >= 1 && source.report_type <= 3 &&
          context->report_map->find_report(kind, source.report_id) == nullptr) {
        ESP_LOGV(TAG,
                 "HID_SCHEMA_WARNING service_instance=%u handle=%u report_id=%u report_type=%u "
                 "reason=reference_not_defined",
                 service.instance, source.characteristic_handle, source.report_id, source.report_type);
        this->degraded_ = true;
      }
    }
    ESP_LOGV(TAG,
             "HID_PROFILE service_instance=%u report_map_sha256=%s reports=%u fields=%u collections=%u protocol=%s",
             context->service_instance, context->report_map_hash.c_str(),
             static_cast<unsigned>(context->report_map->reports().size()),
             static_cast<unsigned>(context->report_map->fields().size()),
             static_cast<unsigned>(context->report_map->collections().size()),
             context->protocol_mode == 0   ? "boot"
             : context->protocol_mode == 1 ? "report"
                                           : "unknown");
  }
}

void BLEClientHID::start_protocol_mode_write_() {
  if (this->setup_phase_ == SetupPhase::BUILDING_PROFILE) {
    this->protocol_write_handles_.clear();
    if (this->protocol_mode_policy_ != ProtocolModePolicy::UNCHANGED) {
      for (const auto &context : this->hid_services_) {
        if (context.protocol_mode_handle != 0)
          this->protocol_write_handles_.push_back(context.protocol_mode_handle);
      }
    }
    this->transition_(SetupPhase::WRITING_PROTOCOL_MODE);
  }
  if (this->protocol_write_handles_.empty()) {
    this->finalize_setup_();
    return;
  }
  const uint16_t handle = this->protocol_write_handles_.front();
  this->protocol_write_handles_.pop_front();
  uint8_t value = this->protocol_mode_policy_ == ProtocolModePolicy::BOOT ? 0 : 1;
  const esp_err_t status =
      esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle, 1, &value,
                               ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status == ESP_OK) {
    this->operations_succeeded_++;
    for (auto &context : this->hid_services_) {
      if (context.protocol_mode_handle == handle)
        context.protocol_mode = value;
    }
    ESP_LOGV(TAG, "HID_PROTOCOL_WRITE handle=%u requested_mode=%s status=scheduled", handle,
             value == 0 ? "boot" : "report");
  } else {
    this->mark_degraded_("Protocol Mode write failed");
  }
}

// -- Subscriptions -----------------------------------------------------------------------

void BLEClientHID::plan_subscriptions_() {
  this->subscription_queue_.clear();
  for (const auto &service : this->services_) {
    for (const auto &characteristic : service.characteristics) {
      const uint16_t uuid = uuid16_of(characteristic.uuid);
      const bool battery = uuid16_(service.uuid, UUID_BATTERY_SERVICE) && uuid == UUID_BATTERY_LEVEL;
      const bool readable = (characteristic.properties & ESP_GATT_CHAR_PROP_BIT_READ) != 0;
      // The keepalive reads the battery level, or else the (small, always readable) device name.
      if (battery && readable) {
        this->keepalive_handle_ = characteristic.handle;
        this->battery_handles_.insert(characteristic.handle);
      } else if (this->keepalive_handle_ == 0 && readable && uuid16_(service.uuid, UUID_GAP_SERVICE) &&
                 uuid == UUID_DEVICE_NAME) {
        this->keepalive_handle_ = characteristic.handle;
      }
      if ((characteristic.properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) == 0)
        continue;
      const bool service_changed = uuid16_(service.uuid, UUID_GATT_SERVICE) && uuid == UUID_SERVICE_CHANGED;
      const bool hid = uuid16_(service.uuid, UUID_HID_SERVICE) &&
                       (uuid == UUID_HID_REPORT || uuid == UUID_BOOT_KEYBOARD_INPUT || uuid == UUID_BOOT_MOUSE_INPUT);
      if (!battery && !service_changed && !hid && this->discovery_mode_ != DiscoveryMode::FORENSIC)
        continue;
      if (this->subscription_queue_.size() >= MAX_SUBSCRIPTIONS) {
        this->mark_degraded_("subscription limit exceeded");
        break;
      }
      SubscriptionInfo subscription;
      subscription.characteristic_handle = characteristic.handle;
      subscription.service_instance = service.instance;
      subscription.properties = characteristic.properties;
      subscription.required = hid;
      const GattDescriptorInfo *cccd = this->find_descriptor_(characteristic, UUID_CCCD);
      if (cccd != nullptr)
        subscription.cccd_handle = cccd->handle;
      // Input reports first, they are what the user is waiting for.
      if (hid) {
        this->subscription_queue_.push_front(subscription);
        this->hid_report_handles_.insert(characteristic.handle);
      } else {
        this->subscription_queue_.push_back(subscription);
      }
      if (battery)
        this->battery_handles_.insert(characteristic.handle);
      if (service_changed)
        this->service_changed_handles_.insert(characteristic.handle);
    }
  }
  ESP_LOGV(TAG, "HID_SUBSCRIPTION_PLAN count=%u mode=%s", static_cast<unsigned>(this->subscription_queue_.size()),
           this->discovery_mode_ == DiscoveryMode::FORENSIC ? "forensic" : "standard");
}

void BLEClientHID::start_next_subscription_() {
  if (this->active_subscription_ != nullptr || this->congested_)
    return;
  if (this->subscription_queue_.empty()) {
    this->begin_reads_();
    return;
  }
  this->active_subscription_ = std::make_unique<SubscriptionInfo>(this->subscription_queue_.front());
  this->subscription_queue_.pop_front();
  this->subscription_state_.begin(this->active_subscription_->characteristic_handle,
                                  this->active_subscription_->cccd_handle);
  if (this->active_subscription_->cccd_handle == 0) {
    ESP_LOGV(TAG, "HID_SUBSCRIPTION handle=%u status=failed reason=missing_cccd",
             this->active_subscription_->characteristic_handle);
    if (this->active_subscription_->required) {
      this->mark_degraded_("required notification has no CCCD");
    } else {
      this->operations_failed_++;
    }
    this->active_subscription_.reset();
    this->subscription_state_.reset();
    return;
  }
  // Through the parent, so it holds the GATT cache until the registration completes: its
  // REG_FOR_NOTIFY handler looks up the CCCD there and writes it.
  const esp_err_t status = this->parent()->register_for_notify(this->active_subscription_->characteristic_handle);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "HID_SUBSCRIPTION handle=%u status=registration_schedule_failed code=%d",
             this->active_subscription_->characteristic_handle, status);
    if (this->active_subscription_->required) {
      this->mark_degraded_("required notification registration failed");
    } else {
      this->operations_failed_++;
    }
    this->active_subscription_.reset();
    this->subscription_state_.reset();
  }
}

void BLEClientHID::handle_registration_result_(uint16_t handle, esp_gatt_status_t status) {
  const SubscriptionDecision decision =
      this->subscription_state_.registration_result(handle, status == ESP_GATT_OK, millis(), CCCD_FALLBACK_DELAY_MS);
  if (this->active_subscription_ == nullptr || decision == SubscriptionDecision::UNMATCHED) {
    ESP_LOGV(TAG, "HID_SUBSCRIPTION handle=%u status=unexpected_registration_callback", handle);
    return;
  }
  if (decision == SubscriptionDecision::FAILED) {
    ESP_LOGW(TAG, "HID_SUBSCRIPTION handle=%u status=registration_failed code=%d", handle, status);
    if (this->active_subscription_->required) {
      this->mark_degraded_("required notification registration failed");
    } else {
      this->operations_failed_++;
    }
    this->active_subscription_.reset();
    this->subscription_state_.reset();
    this->transition_(SetupPhase::REGISTERING);
    return;
  }
  ESP_LOGV(TAG, "HID_SUBSCRIPTION handle=%u status=registered cccd_handle=%u", handle,
           this->active_subscription_->cccd_handle);
  this->transition_(SetupPhase::WAITING_FOR_CCCD);
}

void BLEClientHID::write_current_cccd_() {
  if (this->active_subscription_ == nullptr)
    return;
  this->subscription_state_.own_cccd_write_attempted();
  uint16_t value = (this->active_subscription_->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0 ? 1 : 2;
  const esp_err_t status = esp_ble_gattc_write_char_descr(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), this->active_subscription_->cccd_handle,
      sizeof(value), reinterpret_cast<uint8_t *>(&value), ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "HID_SUBSCRIPTION handle=%u cccd_handle=%u status=cccd_schedule_failed code=%d",
             this->active_subscription_->characteristic_handle, this->active_subscription_->cccd_handle, status);
    if (this->active_subscription_->required) {
      this->mark_degraded_("required CCCD write failed");
    } else {
      this->operations_failed_++;
    }
    this->active_subscription_.reset();
    this->subscription_state_.reset();
    this->transition_(SetupPhase::REGISTERING);
  }
}

void BLEClientHID::handle_cccd_result_(uint16_t handle, esp_gatt_status_t status) {
  const SubscriptionDecision decision = this->subscription_state_.cccd_result(handle, status == ESP_GATT_OK);
  if (this->active_subscription_ == nullptr || decision == SubscriptionDecision::UNMATCHED) {
    ESP_LOGV(TAG, "HID_SUBSCRIPTION cccd_handle=%u status=unmatched_write_callback code=%d", handle, status);
    return;
  }
  if (decision == SubscriptionDecision::COMPLETE) {
    ESP_LOGV(TAG, "HID_SUBSCRIPTION handle=%u cccd_handle=%u status=enabled",
             this->active_subscription_->characteristic_handle, handle);
    this->operations_succeeded_++;
    this->active_subscription_.reset();
    this->subscription_state_.reset();
    this->transition_(SetupPhase::REGISTERING);
    return;
  }
  if (decision == SubscriptionDecision::RETRY_CCCD) {
    ESP_LOGV(TAG, "HID_SUBSCRIPTION handle=%u cccd_handle=%u status=framework_write_failed code=%d action=retry",
             this->active_subscription_->characteristic_handle, handle, status);
    this->write_current_cccd_();
    return;
  }
  ESP_LOGW(TAG, "HID_SUBSCRIPTION handle=%u cccd_handle=%u status=failed code=%d",
           this->active_subscription_->characteristic_handle, handle, status);
  if (this->active_subscription_->required) {
    this->mark_degraded_("required CCCD write failed");
  } else {
    this->operations_failed_++;
  }
  this->active_subscription_.reset();
  this->subscription_state_.reset();
  this->transition_(SetupPhase::REGISTERING);
}

// -- Ready -------------------------------------------------------------------------------

void BLEClientHID::finalize_setup_() {
  this->profile_ready_ = true;
  if (!this->hid_connected_) {
    this->hid_connected_ = true;
#ifdef USE_BT_PAIRING
    if (this->pairing_ != nullptr)
      this->pairing_->on_connection(this, true);
#endif
  }
  this->update_conn_params_();
  if (this->degraded_) {
    this->transition_(SetupPhase::DEGRADED);
  } else {
    this->status_clear_warning();
    this->transition_(SetupPhase::READY);
  }
  this->gamepad_attach_(hid::HIDBus::BLE, this->pnp_vid_, this->pnp_pid_, this->pnp_version_);
  // Without a Firmware Revision String: the PnP ID's release (BCD).
  if (this->get_details().firmware[0] == '\0' && this->pnp_version_ != 0) {
    char release[8];
    snprintf(release, sizeof(release), "%X.%02X", this->pnp_version_ >> 8, this->pnp_version_ & 0xFF);
    this->set_firmware(release);
  }
  this->gamepad_start_();
  this->end_pairing_(true, nullptr);
  // Cache the profile for the next connection, if every HID service has its report map.
  bool complete = !this->hid_services_.empty();
  for (const auto &context : this->hid_services_)
    complete = complete && context.report_map != nullptr;
  this->cache_valid_ = complete;
  this->cached_profile_hash_ = complete ? this->gatt_profile_hash_ : std::string();
  ESP_LOGV(TAG,
           "HID_READY status=%s start=%s services=%u hid_services=%u operations_succeeded=%u operations_failed=%u "
           "gatt_profile=%s",
           this->degraded_ ? "DEGRADED" : "OK", this->warm_start_ ? "warm" : "cold",
           static_cast<unsigned>(this->services_.size()), static_cast<unsigned>(this->hid_services_.size()),
           static_cast<unsigned>(this->operations_succeeded_), static_cast<unsigned>(this->operations_failed_),
           this->gatt_profile_hash_.c_str());
  this->last_activity_ms_ = millis();
  // The parent frees its copy of the GATT database once every node is established; from here
  // on only reads and writes by handle are made.
  this->node_state = espbt::ClientState::ESTABLISHED;
  this->write_leds_(this->leds_);
}

// Limits from Bluetooth Core Spec Vol 3 Part C 12.3 and Vol 4 Part E 7.8.18; 0xFFFF in a field
// means no preference.
bool BLEClientHID::validate_preferred_conn_params_() {
  esp_ble_conn_update_params_t &p = this->preferred_conn_params_;
  if (p.min_int == 0xFFFF || p.max_int == 0xFFFF || p.timeout == 0xFFFF) {
    ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS preferred=none");
    return false;
  }
  if (p.min_int < 0x0006 || p.max_int > 0x0C80 || p.min_int > p.max_int || p.timeout < 0x000A || p.timeout > 0x0C80 ||
      p.latency > 0x01F3) {
    ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS preferred=invalid interval=%u-%u latency=%u timeout=%u", p.min_int,
             p.max_int, p.latency, p.timeout);
    return false;
  }
  // timeout (10 ms units) * 4 > (1 + latency) * max interval (1.25 ms units)
  const uint32_t max_latency_plus_one = static_cast<uint32_t>(p.timeout) * 4 / p.max_int;
  if (max_latency_plus_one <= 1) {
    ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS preferred=invalid reason=timeout_too_short");
    return false;
  }
  uint16_t max_latency = max_latency_plus_one - 1;
  // A few missed connection events must not drop the link.
  if (max_latency > 0)
    max_latency--;
  if (p.latency > max_latency) {
    ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS latency=%u clamped_to=%u", p.latency, max_latency);
    p.latency = max_latency;
  }
  return true;
}

bool BLEClientHID::build_desired_conn_params_(esp_ble_conn_update_params_t &out) {
  memset(&out, 0, sizeof(out));
  if (this->preferred_conn_params_valid_) {
    out = this->preferred_conn_params_;
  } else if (this->has_conn_latency_) {
    esp_gap_conn_params_t current;
    if (esp_ble_get_current_conn_params(this->parent()->get_remote_bda(), &current) != ESP_OK)
      return false;
    out.min_int = current.interval;
    out.max_int = current.interval;
    out.latency = current.latency;
    out.timeout = current.timeout;
  } else {
    return false;
  }
  if (out.max_int == 0)
    return false;
  if (this->has_conn_latency_) {
    out.latency = this->conn_latency_;
    // The link must survive a whole latency window: timeout > (1 + latency) * max interval * 2,
    // in units (1 + latency) * max_int / 4, with 50 % headroom.
    uint32_t required = ((1u + out.latency) * out.max_int * 3u + 7u) / 8u;
    required = std::max<uint32_t>(required, 10u);
    if (required > 3200u) {
      ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS conn_latency=%u too large for interval %.2f ms, ignored", out.latency,
               out.max_int * 1.25f);
      out.latency = 0;
    } else if (out.timeout < required) {
      out.timeout = static_cast<uint16_t>(required);
    }
  }
  memcpy(out.bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t));
  return true;
}

// The result is not awaited: esp32_ble consumes ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT (and logs a
// failed update) without forwarding it to client nodes, and the link works on the old
// parameters either way. The live parameters are read back and logged a little later.
void BLEClientHID::update_conn_params_() {
  esp_ble_conn_update_params_t desired;
  if (!this->build_desired_conn_params_(desired))
    return;
  esp_gap_conn_params_t current;
  if (esp_ble_get_current_conn_params(this->parent()->get_remote_bda(), &current) == ESP_OK &&
      current.interval >= desired.min_int && current.interval <= desired.max_int &&
      current.latency == desired.latency && current.timeout == desired.timeout) {
    ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS status=unchanged");
    return;
  }
  ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS status=requested interval_ms=%.2f-%.2f latency=%u timeout_ms=%.1f",
           desired.min_int * 1.25f, desired.max_int * 1.25f, desired.latency, desired.timeout * 10.0f);
  if (esp_ble_gap_update_conn_params(&desired) != ESP_OK) {
    ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS status=request_failed");
    return;
  }
  this->set_timeout("conn_params", CONN_PARAMS_SETTLE_MS, [this]() {
    esp_gap_conn_params_t params;
    if (esp_ble_get_current_conn_params(this->parent()->get_remote_bda(), &params) == ESP_OK) {
      ESP_LOGV(TAG, "HID_CONNECTION_PARAMETERS status=active interval_ms=%.2f latency=%u timeout_ms=%.1f",
               params.interval * 1.25f, params.latency, params.timeout * 10.0f);
    }
  });
}

void BLEClientHID::set_keepalive_enabled(bool enabled) {
  if (enabled == this->keepalive_enabled_)
    return;
  this->keepalive_enabled_ = enabled;
  if (this->keepalive_interval_ms_ == 0)
    return;
  if (enabled) {
    // Whatever enabled it was activity too.
    this->last_activity_ms_ = millis();
    ESP_LOGV(TAG, "HID_KEEPALIVE status=enabled interval_ms=%" PRIu32, this->keepalive_interval_ms_);
  } else {
    ESP_LOGV(TAG, "HID_KEEPALIVE status=disabled");
    // Disconnected from here on is the intended state.
    if (this->setup_phase_ == SetupPhase::DISCONNECTED)
      this->status_clear_warning();
  }
}

void BLEClientHID::maybe_send_keepalive_() {
  if (!this->keepalive_enabled_ || this->keepalive_interval_ms_ == 0 || this->keepalive_handle_ == 0 ||
      this->active_read_ != nullptr)
    return;
  const uint32_t now = millis();
  if (!elapsed(now, this->last_activity_ms_, this->keepalive_interval_ms_))
    return;
  this->last_activity_ms_ = now;
  auto operation = std::make_unique<GattReadOperation>();
  operation->handle = this->keepalive_handle_;
  operation->characteristic_handle = this->keepalive_handle_;
  operation->purpose = ReadPurpose::KEEPALIVE;
  const esp_err_t status = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                                   this->keepalive_handle_, this->read_auth_());
  if (status != ESP_OK) {
    ESP_LOGV(TAG, "HID_KEEPALIVE status=read_failed code=%d", status);
    return;
  }
  this->active_read_ = std::move(operation);
}

// -- Lock LEDs ---------------------------------------------------------------------------

void BLEClientHID::write_leds_(uint8_t leds) {
  if (this->setup_phase_ != SetupPhase::READY && this->setup_phase_ != SetupPhase::DEGRADED)
    return;
  for (const auto &context : this->hid_services_) {
    uint16_t handle = 0;
    std::vector<uint8_t> payload;
    if (context.protocol_mode == 0) {
      // Boot protocol: the one-byte Boot Keyboard Output Report.
      for (const auto &service : this->services_) {
        if (service.instance != context.service_instance)
          continue;
        for (const auto &characteristic : service.characteristics) {
          if (uuid16_(characteristic.uuid, UUID_BOOT_KEYBOARD_OUTPUT))
            handle = characteristic.handle;
        }
      }
      payload.assign(1, static_cast<uint8_t>(leds & (hid::LED_NUM_LOCK | hid::LED_CAPS_LOCK | hid::LED_SCROLL_LOCK)));
    } else if (context.report_map != nullptr) {
      uint8_t report_id = 0;
      if (!hid::build_led_report(*context.report_map, leds, report_id, payload))
        continue;
      // The characteristic of an output report, by its Report Reference. HOGP leaves the
      // report ID out of the value.
      for (const auto &entry : this->handle_report_source_) {
        const HIDReportSource &source = entry.second;
        if (source.service_instance == context.service_instance && source.has_report_type &&
            source.report_type == static_cast<uint8_t>(HIDReportKind::OUTPUT) &&
            (!context.report_map->uses_report_ids() || (source.has_report_id && source.report_id == report_id))) {
          handle = entry.first;
          break;
        }
      }
    }
    if (handle == 0 || payload.empty())
      continue;
    const GattCharacteristicInfo *characteristic = this->find_characteristic_(handle);
    const bool with_response =
        characteristic == nullptr || (characteristic->properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0;
    const esp_err_t status = esp_ble_gattc_write_char(
        this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle, payload.size(), payload.data(),
        with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (status != ESP_OK)
      ESP_LOGV(TAG, "HID_LEDS handle=%u status=write_failed code=%d", handle, status);
    return;
  }
}

// -- Gamepad driver transport --------------------------------------------------------------

uint16_t BLEClientHID::report_handle_(hid::HIDReportKind kind, const uint8_t *data, size_t len, bool &with_id) const {
  if (len == 0)
    return 0;
  for (const auto &entry : this->handle_report_source_) {
    const HIDReportSource &source = entry.second;
    if (!source.has_report_type || source.report_type != static_cast<uint8_t>(kind))
      continue;
    const HIDServiceContext *context = this->find_hid_context_(source.service_instance);
    with_id = context != nullptr && context->report_map != nullptr && context->report_map->uses_report_ids();
    if (!with_id || (source.has_report_id && source.report_id == data[0]))
      return entry.first;
  }
  return 0;
}

// HOGP leaves the report ID out of the value.
bool BLEClientHID::write_report_(uint16_t handle, const uint8_t *data, size_t len) {
  if (handle == 0 || (this->setup_phase_ != SetupPhase::READY && this->setup_phase_ != SetupPhase::DEGRADED))
    return false;
  const GattCharacteristicInfo *characteristic = this->find_characteristic_(handle);
  const bool with_response =
      characteristic == nullptr || (characteristic->properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0;
  return esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle, len,
                                  const_cast<uint8_t *>(data),
                                  with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
                                  ESP_GATT_AUTH_REQ_NONE) == ESP_OK;
}

bool BLEClientHID::send_output_report(const uint8_t *data, size_t len) {
  return this->set_report(hid::HIDReportKind::OUTPUT, data, len);
}

bool BLEClientHID::set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) {
  bool with_id = false;
  const uint16_t handle = this->report_handle_(kind, data, len, with_id);
  return with_id ? this->write_report_(handle, data + 1, len - 1) : this->write_report_(handle, data, len);
}

bool BLEClientHID::request_feature_report(uint8_t id, uint16_t len) {
  if (this->feature_read_handle_ != 0 ||
      (this->setup_phase_ != SetupPhase::READY && this->setup_phase_ != SetupPhase::DEGRADED))
    return false;
  bool with_id = false;
  const uint16_t handle = this->report_handle_(hid::HIDReportKind::FEATURE, &id, 1, with_id);
  if (handle == 0 || esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle,
                                             this->read_auth_()) != ESP_OK)
    return false;
  this->feature_read_handle_ = handle;
  this->feature_read_id_ = id;
  return true;
}

// -- GAP events --------------------------------------------------------------------------

bool BLEClientHID::request_signal() {
  if (this->node_state != espbt::ClientState::ESTABLISHED)
    return false;
  return esp_ble_gap_read_rssi(this->parent()->get_remote_bda()) == ESP_OK;
}

void BLEClientHID::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT) {
    if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS &&
        memcmp(param->read_rssi_cmpl.remote_addr, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) == 0)
      this->set_signal(param->read_rssi_cmpl.rssi, false);
    return;
  }
  if ((event == ESP_GAP_BLE_PASSKEY_NOTIF_EVT || event == ESP_GAP_BLE_NC_REQ_EVT) &&
      memcmp(param->ble_security.key_notif.bd_addr, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) == 0) {
    if (event == ESP_GAP_BLE_NC_REQ_EVT) {
      // Numeric comparison: the device shows the number and confirms it; nothing to compare here.
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
      return;
    }
    ESP_LOGI(TAG, "[%s] Passkey: type %06" PRIu32 " and Enter on the device", this->parent()->address_str(),
             param->ble_security.key_notif.passkey);
#ifdef USE_BT_PAIRING
    if (this->pairing_ != nullptr)
      this->pairing_->on_passkey(this, param->ble_security.key_notif.passkey);
#endif
    return;
  }
  if (event != ESP_GAP_BLE_AUTH_CMPL_EVT ||
      memcmp(param->ble_security.auth_cmpl.bd_addr, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) != 0)
    return;
  this->restore_io_capability_();
  // A device without pairing support is used unencrypted, now and on every reconnect.
  if (param->ble_security.auth_cmpl.success ||
      param->ble_security.auth_cmpl.fail_reason != ESP_AUTH_SMP_PAIR_NOT_SUPPORT)
    return;
  this->plain_address_ = this->parent()->get_address();
  ESP_LOGV(TAG, "[%s] Pairing not supported by the device, continuing unencrypted", this->parent()->address_str());
}

// Bluedroid reads the IO capability when a pairing starts (bta_dm_co_ble_io_req). The configured
// one is offered to a keyboard, which can type a passkey; any other device (a remote, a mouse)
// pairs as with no input and output, since it may report keys it cannot type a passkey with.
void BLEClientHID::apply_io_capability_() {
  if (esp32_ble::global_ble == nullptr)
    return;
  const esp_ble_io_cap_t configured = esp32_ble::global_ble->get_io_capability();
  if (configured == ESP_IO_CAP_NONE)
    return;
  uint16_t appearance = 0;
  if (this->pairing_active_ && this->candidate_selected_) {
    appearance = this->candidate_appearance_;
#ifdef USE_BT_PAIRING
  } else if (this->pairing_ != nullptr) {
    if (const auto *record = this->pairing_->find_slot(this->slot_); record != nullptr)
      appearance = static_cast<uint16_t>(record->device_class);
#endif
  }
  if (appearance == APPEARANCE_KEYBOARD)
    return;
  esp_ble_io_cap_t none = ESP_IO_CAP_NONE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &none, sizeof(none));
  this->io_cap_lowered_ = true;
  ESP_LOGV(TAG, "[%s] Not a keyboard (appearance 0x%04X): pairing without passkey", this->parent()->address_str(),
           appearance);
}

void BLEClientHID::restore_io_capability_() {
  if (!this->io_cap_lowered_ || esp32_ble::global_ble == nullptr)
    return;
  this->io_cap_lowered_ = false;
  esp_ble_io_cap_t configured = esp32_ble::global_ble->get_io_capability();
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &configured, sizeof(configured));
}

// -- GATT events -------------------------------------------------------------------------

void BLEClientHID::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  if (param == nullptr || (gattc_if != ESP_GATT_IF_NONE && gattc_if != this->parent()->get_gattc_if()))
    return;
  switch (event) {
    case ESP_GATTC_CONNECT_EVT:
      if (memcmp(param->connect.remote_bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) != 0)
        break;
      this->reset_connection_state_();
      this->transition_(SetupPhase::WAITING_FOR_DISCOVERY);
      if (!this->plain_()) {
        this->apply_io_capability_();
        if (esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT) != ESP_OK)
          ESP_LOGW(TAG, "HID_CONNECTION security=request_failed");
      }
      break;
    case ESP_GATTC_DISCONNECT_EVT: {
      if (memcmp(param->disconnect.remote_bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) != 0)
        break;
      this->restore_io_capability_();
      // A peripheral that is allowed to sleep drops the link; that is normal operation. Only a
      // disconnect while nothing lets it sleep is a fault.
      const bool expected = this->keepalive_interval_ms_ != 0 && !this->keepalive_enabled_;
      if (expected) {
        ESP_LOGV(TAG, "HID_CONNECTION status=disconnected reason=%d expected=true", param->disconnect.reason);
      } else {
        ESP_LOGI(TAG, "HID_CONNECTION status=disconnected reason=%d", param->disconnect.reason);
        this->status_set_warning("Disconnected");
      }
      this->feature_read_handle_ = 0;
      this->gamepad_detach_();
      this->release_all_hid();
      this->reset_connection_state_();
      this->transition_(SetupPhase::DISCONNECTED);
      if (this->hid_connected_) {
        this->hid_connected_ = false;
#ifdef USE_BT_PAIRING
        if (this->pairing_ != nullptr)
          this->pairing_->on_connection(this, false);
#endif
      }
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
      if (param->search_cmpl.conn_id != this->parent()->get_conn_id() ||
          this->setup_phase_ != SetupPhase::WAITING_FOR_DISCOVERY)
        break;
      if (param->search_cmpl.status != ESP_GATT_OK) {
        this->mark_degraded_("GATT service discovery failed");
        this->finalize_setup_();
      } else {
        this->transition_(SetupPhase::INVENTORY);
      }
      break;
    case ESP_GATTC_READ_CHAR_EVT:
    case ESP_GATTC_READ_DESCR_EVT:
      if (param->read.conn_id != this->parent()->get_conn_id())
        break;
      if (event == ESP_GATTC_READ_CHAR_EVT && this->feature_read_handle_ != 0 &&
          param->read.handle == this->feature_read_handle_) {
        // A gamepad driver's feature report, given to it with its report ID first.
        this->feature_read_handle_ = 0;
        std::vector<uint8_t> report;
        if (param->read.status == ESP_GATT_OK) {
          report.push_back(this->feature_read_id_);
          report.insert(report.end(), param->read.value, param->read.value + param->read.value_len);
        }
        this->gamepad_feature_(this->feature_read_id_, report.data(), report.size());
        break;
      }
      if (this->active_read_ == nullptr || this->active_read_->handle != param->read.handle) {
        ESP_LOGV(TAG, "GATT_READ handle=%u status=unmatched_callback", param->read.handle);
        break;
      }
      this->finish_active_read_(param->read.status, param->read.value, param->read.value_len);
      break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      // Carries no conn_id; matched by handle against the subscription in progress.
      this->handle_registration_result_(param->reg_for_notify.handle, param->reg_for_notify.status);
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      if (param->write.conn_id == this->parent()->get_conn_id())
        this->handle_cccd_result_(param->write.handle, param->write.status);
      break;
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.conn_id != this->parent()->get_conn_id() ||
          memcmp(param->notify.remote_bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) != 0)
        break;
      this->last_activity_ms_ = millis();
      const uint64_t seq_id = this->log_raw_notification_(param->notify);
      if (this->service_changed_handles_.count(param->notify.handle) != 0) {
        ESP_LOGV(TAG, "HID_SERVICE_CHANGED seq_id=%" PRIu64 " action=invalidate_profile", seq_id);
        this->request_rediscovery_();
      } else if (this->battery_handles_.count(param->notify.handle) != 0) {
        if (param->notify.value == nullptr || param->notify.value_len < 1) {
          ESP_LOGV(TAG, "HID_IGNORE seq_id=%" PRIu64 " reason=invalid_battery_payload", seq_id);
        } else {
          ESP_LOGV(TAG, "HID_BATTERY source=notification value=%u", param->notify.value[0]);
          this->set_battery(static_cast<int8_t>(std::min<uint8_t>(param->notify.value[0], 100)),
                            this->get_details().battery_state);
#ifdef USE_SENSOR
          if (this->battery_sensor_ != nullptr)
            this->battery_sensor_->publish_state(param->notify.value[0]);
#endif
        }
      } else if (this->hid_report_handles_.count(param->notify.handle) != 0) {
        this->enqueue_input_report_(param->notify, seq_id);
      } else {
        ESP_LOGV(TAG, "HID_RAW_HANDLING seq_id=%" PRIu64 " result=raw_only reason=no_decoder", seq_id);
      }
      break;
    }
    case ESP_GATTC_SRVC_CHG_EVT:
      if (memcmp(param->srvc_chg.remote_bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) == 0)
        this->request_rediscovery_();
      break;
    case ESP_GATTC_CONGEST_EVT:
      if (param->congest.conn_id == this->parent()->get_conn_id()) {
        this->congested_ = param->congest.congested;
        ESP_LOGV(TAG, "GATT_CONGESTION congested=%s", this->congested_ ? "true" : "false");
      }
      break;
    case ESP_GATTC_QUEUE_FULL_EVT:
      if (param->queue_full.conn_id == this->parent()->get_conn_id()) {
        this->congested_ = param->queue_full.is_full;
        ESP_LOGV(TAG, "GATT_QUEUE_FULL full=%s status=%d", param->queue_full.is_full ? "true" : "false",
                 param->queue_full.status);
      }
      break;
    default:
      break;
  }
}

// The database changed: the cached profile no longer applies, and the link is set up again.
void BLEClientHID::request_rediscovery_() {
  this->cache_valid_ = false;
  this->cached_profile_hash_.clear();
  this->rediscovery_requested_ = true;
}

// -- Input reports -----------------------------------------------------------------------

uint64_t BLEClientHID::log_raw_notification_(const esp_ble_gattc_cb_param_t::gattc_notify_evt_param &notification) {
  const uint64_t seq_id = this->next_seq_id_++;
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  const uint16_t handle = notification.handle;
  const char *classification = this->battery_handles_.count(handle) != 0           ? "BATTERY"
                               : this->service_changed_handles_.count(handle) != 0 ? "SERVICE_CHANGED"
                               : this->hid_report_handles_.count(handle) != 0      ? "HID"
                                                                                   : "UNKNOWN";
  HIDReportSource source;
  auto known = this->handle_report_source_.find(handle);
  if (known != this->handle_report_source_.end())
    source = known->second;
  const std::string report_id = source.has_report_id ? std::to_string(source.report_id) : "-";
  const char *report_type = source.has_report_type && source.report_type >= 1 && source.report_type <= 3
                                ? hid_report_kind_name(static_cast<HIDReportKind>(source.report_type))
                                : "-";
  ESP_LOGV(TAG, "HID_RAW seq_id=%" PRIu64 " class=%s handle=%u report_id=%s report_type=%s transport=%s len=%u data=%s",
           seq_id, classification, handle, report_id.c_str(), report_type,
           notification.is_notify ? "notify" : "indicate", notification.value_len,
           format_value_(notification.value, notification.value_len).c_str());
#endif
  return seq_id;
}

void BLEClientHID::enqueue_input_report_(const esp_ble_gattc_cb_param_t::gattc_notify_evt_param &notification,
                                         uint64_t seq_id) {
  if (notification.value == nullptr && notification.value_len != 0) {
    ESP_LOGV(TAG, "HID_IGNORE seq_id=%" PRIu64 " reason=null_payload len=%u", seq_id, notification.value_len);
    return;
  }
  if (notification.value_len > MAX_REPORT_PAYLOAD_BYTES) {
    ESP_LOGV(TAG, "HID_IGNORE seq_id=%" PRIu64 " reason=payload_limit len=%u", seq_id, notification.value_len);
    return;
  }
  while (!this->pending_reports_.empty() &&
         (this->pending_reports_.size() >= MAX_PENDING_REPORTS ||
          this->pending_report_bytes_ + notification.value_len > MAX_PENDING_REPORT_BYTES)) {
    this->reports_dropped_++;
    const uint64_t dropped = this->pending_reports_.front().seq_id;
    this->pending_report_bytes_ -= this->pending_reports_.front().payload.size();
    this->pending_reports_.pop_front();
    const uint32_t now = millis();
    if (this->last_overflow_warning_ == 0 || elapsed(now, this->last_overflow_warning_, OVERFLOW_WARNING_INTERVAL_MS)) {
      ESP_LOGW(TAG, "HID_QUEUE status=full dropped_total=%" PRIu32 " dropped_seq_id=%" PRIu64 " policy=drop_oldest",
               this->reports_dropped_, dropped);
      this->last_overflow_warning_ = now;
    }
  }
  PendingHIDReport report;
  report.handle = notification.handle;
  report.is_notification = notification.is_notify;
  report.seq_id = seq_id;
  if (notification.value_len != 0)
    report.payload.assign(notification.value, notification.value + notification.value_len);
  this->pending_report_bytes_ += report.payload.size();
  this->pending_reports_.push_back(std::move(report));
}

bool BLEClientHID::events_wanted_() const {
#ifdef USE_API_HOMEASSISTANT_SERVICES
  if (this->publish_to_ha_ && api::global_api_server->has_homeassistant_action_subscriber())
    return true;
#endif
#ifdef USE_TEXT_SENSOR
  if (this->last_event_usage_text_sensor_ != nullptr || this->last_event_code_text_sensor_ != nullptr)
    return true;
#endif
#ifdef USE_SENSOR
  if (this->last_event_value_sensor_ != nullptr)
    return true;
#endif
  return false;
}

// The report each handle carries is looked up here rather than when the notification arrived:
// a report that came in during setup is only known once the Report References are read.
void BLEClientHID::process_input_report_(const PendingHIDReport &report) {
  HIDReportSource source;
  auto known = this->handle_report_source_.find(report.handle);
  if (known != this->handle_report_source_.end())
    source = known->second;
  source.characteristic_handle = report.handle;
  source.is_notification = report.is_notification;
  source.has_transport = true;

  HIDDecodeStatus status = HIDDecodeStatus::SCHEMA_MISSING;
  bool recognized = false;
  std::vector<HIDReportItemValue> values;
  if (source.characteristic_uuid == UUID_BOOT_KEYBOARD_INPUT || source.characteristic_uuid == UUID_BOOT_MOUSE_INPUT) {
    values = this->boot_decoder_.decode(source, report.payload.data(), report.payload.size(), &status);
    recognized = status == HIDDecodeStatus::EXACT || status == HIDDecodeStatus::LONG;
  } else {
    HIDServiceContext *context = this->find_hid_context_(source.service_instance);
    // A gamepad driver reads the report as the device sends it over USB: report ID first.
    if (context != nullptr && context->report_map != nullptr && context->report_map->uses_report_ids() &&
        source.has_report_id) {
      std::vector<uint8_t> numbered;
      numbered.reserve(report.payload.size() + 1);
      numbered.push_back(source.report_id);
      numbered.insert(numbered.end(), report.payload.begin(), report.payload.end());
      if (this->gamepad_report_(numbered.data(), numbered.size()))
        return;
    } else if (this->gamepad_report_(report.payload.data(), report.payload.size())) {
      return;
    }
    if (context != nullptr && context->report_map != nullptr)
      values = context->report_map->parse(source, report.payload.data(), report.payload.size(), &recognized, &status);
  }
  this->process_hid_values(values);
  if (!this->events_wanted_())
    return;
  const std::string raw = format_value_(report.payload.data(), report.payload.size());
  this->process_decoded_values_(report, source, raw, values, status);
  if (values.empty() && recognized) {
    ESP_LOGV(TAG, "HID_NO_EVENT seq_id=%" PRIu64 " reason=unchanged decode_status=%s", report.seq_id,
             hid_decode_status_name(status));
  } else if (values.empty() && !recognized) {
    HIDEvent barrier = this->make_hid_event_(report, source, raw, nullptr, status);
    for (HIDEvent &event : this->sampled_events_.drain_before(std::move(barrier)))
      this->emit_hid_event_(std::move(event));
    this->sampling_window_active_ = false;
  }
}

// -- Home Assistant events and last-event sensors ----------------------------------------

void BLEClientHID::process_decoded_values_(const PendingHIDReport &report, const HIDReportSource &source,
                                           const std::string &raw, const std::vector<HIDReportItemValue> &values,
                                           HIDDecodeStatus status) {
  this->update_sampling_window_();
  size_t index = 0;
  while (index < values.size()) {
    const HIDReportItemValue &value = values[index];
    if (this->event_sampling_interval_us_ == 0 || value.aggregation == HIDReportItemValue::Aggregation::PASSTHROUGH) {
      if (this->event_sampling_interval_us_ != 0) {
        HIDEvent barrier = this->make_hid_event_(report, source, raw, &value, status);
        for (HIDEvent &event : this->sampled_events_.drain_before(std::move(barrier)))
          this->emit_hid_event_(std::move(event));
        this->sampling_window_active_ = false;
      } else {
        this->emit_hid_event_(this->make_hid_event_(report, source, raw, &value, status));
      }
      index++;
      continue;
    }
    size_t run_end = index + 1;
    while (run_end < values.size() && values[run_end].aggregation != HIDReportItemValue::Aggregation::PASSTHROUGH)
      run_end++;
    if (!this->sampling_window_active_) {
      for (size_t current = index; current < run_end; current++)
        this->emit_hid_event_(this->make_hid_event_(report, source, raw, &values[current], status));
      this->sampling_window_active_ = true;
      this->sampling_window_started_us_ = micros();
    } else {
      for (size_t current = index; current < run_end; current++)
        this->accumulate_event_(report, source, raw, values[current], status);
    }
    index = run_end;
  }
}

void BLEClientHID::accumulate_event_(const PendingHIDReport &report, const HIDReportSource &source,
                                     const std::string &raw, const HIDReportItemValue &value, HIDDecodeStatus status) {
  const SampledEventKey key{source.service_instance, value.field_id,
                            static_cast<uint8_t>(source.has_report_id ? source.report_id : 0),
                            static_cast<uint8_t>(HIDReportKind::INPUT)};
  HIDEvent event = this->make_hid_event_(report, source, raw, &value, status);
  if (this->sampled_events_.accumulate(key, value.aggregation, value.value, value.raw_value, std::move(event)))
    return;
  this->flush_sampled_events_("pressure");
  HIDEvent retry = this->make_hid_event_(report, source, raw, &value, status);
  if (!this->sampled_events_.accumulate(key, value.aggregation, value.value, value.raw_value, std::move(retry))) {
    ESP_LOGW(TAG, "HID_COALESCING status=failed service_instance=%u field_id=%" PRIu32, key.service_instance,
             key.field_id);
  }
  this->sampling_window_started_us_ = micros();
}

void BLEClientHID::update_sampling_window_() {
  if (!this->sampling_window_active_ || this->event_sampling_interval_us_ == 0)
    return;
  const uint32_t now = micros();
  if (!elapsed(now, this->sampling_window_started_us_, this->event_sampling_interval_us_))
    return;
  if (this->sampled_events_.empty()) {
    this->sampling_window_active_ = false;
    return;
  }
  this->flush_sampled_events_("timer");
  this->sampling_window_started_us_ = now;
}

void BLEClientHID::flush_sampled_events_(const char *reason) {
  if (this->sampled_events_.empty())
    return;
  ESP_LOGV(TAG, "HID_COALESCING action=flush pending=%u reason=%s", static_cast<unsigned>(this->sampled_events_.size()),
           reason);
  for (HIDEvent &event : this->sampled_events_.drain())
    this->emit_hid_event_(std::move(event));
}

HIDEvent BLEClientHID::make_hid_event_(const PendingHIDReport &report, const HIDReportSource &source,
                                       const std::string &raw, const HIDReportItemValue *value,
                                       HIDDecodeStatus status) {
  const GattCharacteristicInfo *characteristic = this->find_characteristic_(source.characteristic_handle);
  HIDEvent event;
  event.data = {
      {"device", this->parent()->address_str()},
      {"hid_service", std::to_string(source.service_instance)},
      {"handle", std::to_string(source.characteristic_handle)},
      {"characteristic_uuid", characteristic == nullptr ? "unknown" : uuid_string_(characteristic->uuid)},
      {"report_id", source.has_report_id ? std::to_string(source.report_id) : "unknown"},
      {"hid_report_type", source.has_report_type && source.report_type >= 1 && source.report_type <= 3
                              ? hid_report_kind_name(static_cast<HIDReportKind>(source.report_type))
                              : "unknown"},
      {"transport", source.has_transport ? (source.is_notification ? "notification" : "indication") : "unknown"},
      {"length", std::to_string(report.payload.size())},
      {"raw_data", raw},
      {"decode_status", hid_decode_status_name(status)},
      {"seq_id_from", std::to_string(report.seq_id)},
      {"seq_id_to", std::to_string(report.seq_id)},
  };
  const HIDServiceContext *context = this->find_hid_context_(source.service_instance);
  if (context != nullptr && !context->report_map_hash.empty())
    event.data.emplace("profile_id", context->report_map_hash.substr(0, 16));
  if (value == nullptr)
    return event;
  const char *usage_name = lookup_usage_name(value->usage.page, value->usage.usage);
  const std::string usage =
      usage_name == nullptr ? std::to_string(value->usage.page) + "_" + std::to_string(value->usage.usage) : usage_name;
  event.data.emplace("usage", usage);
  event.data.emplace("usage_page", std::to_string(value->usage.page));
  event.data.emplace("usage_id", std::to_string(value->usage.usage));
  event.data.emplace("field_id", std::to_string(value->field_id));
  event.data.emplace("collection_id", value->collection_id == HIDCollectionSchema::NO_PARENT
                                          ? "unknown"
                                          : std::to_string(value->collection_id));
  event.data.emplace("application_usage_page", std::to_string(value->application_usage.page));
  event.data.emplace("application_usage_id", std::to_string(value->application_usage.usage));
  const char *application_name = lookup_usage_name(value->application_usage.page, value->application_usage.usage);
  if (application_name != nullptr)
    event.data.emplace("application_usage", application_name);
  event.data.emplace("value", std::to_string(value->value));
  event.data.emplace("raw_value", std::to_string(value->raw_value));
  event.data.emplace("relative", value->is_relative ? "true" : "false");
  return event;
}

void BLEClientHID::emit_hid_event_(HIDEvent event) {
#ifdef USE_API_HOMEASSISTANT_SERVICES
  if (this->publish_to_ha_ && api::global_api_server->has_homeassistant_action_subscriber())
    this->fire_homeassistant_event("esphome.hid_events", event.data);
#endif
  auto usage = event.data.find("usage");
  if (usage != event.data.end()) {
    // publish_state() runs on_value automations right away: value and code first, so an
    // automation triggered by the usage reads this event's value, not the previous one.
#ifdef USE_SENSOR
    if (this->last_event_value_sensor_ != nullptr)
      this->last_event_value_sensor_->publish_state(strtoll(event.data.at("value").c_str(), nullptr, 10));
#endif
#ifdef USE_TEXT_SENSOR
    if (this->last_event_code_text_sensor_ != nullptr)
      this->last_event_code_text_sensor_->publish_state(event.data.at("usage_page") + "_" + event.data.at("usage_id"));
    if (this->last_event_usage_text_sensor_ != nullptr)
      this->last_event_usage_text_sensor_->publish_state(usage->second);
#endif
  }
  ESP_LOGV(TAG, "HID_EVENT seq_id_from=%s seq_id_to=%s service_instance=%s report_id=%s usage=%s value=%s",
           event.data.at("seq_id_from").c_str(), event.data.at("seq_id_to").c_str(),
           event.data.at("hid_service").c_str(), event.data.at("report_id").c_str(),
           usage != event.data.end() ? usage->second.c_str() : "unknown",
           event.data.count("value") != 0 ? event.data.at("value").c_str() : "unknown");
}

}  // namespace esphome::ble_client_hid

#endif  // USE_ESP32
