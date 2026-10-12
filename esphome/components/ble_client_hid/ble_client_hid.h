#pragma once

// HID over GATT (HOGP) host, from fsievers22/esphome-ble-remote (MIT): pull request #45 by
// Gur Kohli (discovery, report map parsing, Home Assistant events), with the keepalive,
// connection latency and fast reconnect of #47 by Attila Prokai, the connection parameter
// fixes of Jean-Noel Goumain and publish_to_ha of #36. Decoded reports feed hid::HIDInput,
// the same keys, text, pointer and controls usb_hid provides.

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/hid/boot_report_decoder.h"
#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"
#include "esphome/core/component.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_API_HOMEASSISTANT_SERVICES
#include "esphome/components/api/custom_api_device.h"
#endif
#ifdef USE_BT_PAIRING
#include "esphome/components/bt_pairing/bt_pairing.h"
#endif
#include "esphome/core/automation.h"

#include "ordered_event_buffer.h"
#include "subscription_state.h"

namespace esphome::ble_client_hid {

namespace espbt = esphome::esp32_ble_tracker;

using hid::HIDBootReportDecoder;
using hid::HIDCollectionSchema;
using hid::HIDDecodeStatus;
using hid::HIDReportItemValue;
using hid::HIDReportKind;
using hid::HIDReportMap;
using hid::HIDReportSource;
using hid::HIDUsage;

enum class DiscoveryMode : uint8_t { STANDARD, FORENSIC };
enum class ProtocolModePolicy : uint8_t { UNCHANGED, REPORT, BOOT };

// Setup runs INVENTORY -> REGISTERING (input reports first, so the key that woke the device
// is not lost) -> READING -> BUILDING_PROFILE -> WRITING_PROTOCOL_MODE -> READY. A reconnect
// to a device whose GATT database is unchanged reads only the battery level.
enum class SetupPhase : uint8_t {
  DISCONNECTED,
  WAITING_FOR_DISCOVERY,
  INVENTORY,
  REGISTERING,
  WAITING_FOR_CCCD,
  READING,
  BUILDING_PROFILE,
  WRITING_PROTOCOL_MODE,
  READY,
  DEGRADED,
  NO_HID,
};

struct GattDescriptorInfo {
  espbt::ESPBTUUID uuid;
  uint16_t handle{0};
};

struct GattCharacteristicInfo {
  espbt::ESPBTUUID uuid;
  uint16_t handle{0};
  uint8_t properties{0};
  uint16_t service_instance{0};
  std::vector<GattDescriptorInfo> descriptors;
};

struct GattIncludedServiceInfo {
  espbt::ESPBTUUID uuid;
  uint16_t declaration_handle{0};
  uint16_t start_handle{0};
  uint16_t end_handle{0};
};

struct GattServiceInfo {
  espbt::ESPBTUUID uuid;
  uint16_t instance{0};
  uint16_t start_handle{0};
  uint16_t end_handle{0};
  bool primary{true};
  std::vector<GattIncludedServiceInfo> included_services;
  std::vector<GattCharacteristicInfo> characteristics;
};

struct HIDServiceContext {
  uint16_t service_instance{0};
  uint16_t protocol_mode_handle{0};
  uint8_t protocol_mode{0xFF};
  std::string report_map_hash;
  std::unique_ptr<HIDReportMap> report_map;
};

enum class ReadPurpose : uint8_t {
  GENERIC_STANDARD,
  GENERIC_FORENSIC,
  DEVICE_NAME,
  APPEARANCE,
  PREFERRED_CONNECTION_PARAMETERS,
  BATTERY_LEVEL,
  PNP_ID,
  MANUFACTURER,
  SERIAL_NUMBER,
  HID_INFORMATION,
  HID_REPORT_MAP,
  HID_PROTOCOL_MODE,
  HID_REPORT_REFERENCE,
  HID_EXTERNAL_REPORT_REFERENCE,
  HID_REPORT_VALUE,
  KEEPALIVE,
};

struct GattReadOperation {
  uint16_t handle{0};
  uint16_t characteristic_handle{0};
  uint16_t service_instance{0};
  ReadPurpose purpose{ReadPurpose::GENERIC_STANDARD};
  bool descriptor{false};
  bool required{false};
  espbt::ESPBTUUID service_uuid;
  espbt::ESPBTUUID characteristic_uuid;
  espbt::ESPBTUUID attribute_uuid;
};

struct PendingHIDReport {
  uint16_t handle{0};
  bool is_notification{true};
  std::vector<uint8_t> payload;
  uint64_t seq_id{0};
};

struct SubscriptionInfo {
  uint16_t characteristic_handle{0};
  uint16_t cccd_handle{0};
  uint16_t service_instance{0};
  uint8_t properties{0};
  bool required{false};
};

class BLEClientHID : public Component,
                     public ble_client::BLEClientNode,
                     public espbt::ESPBTDeviceListener,
#ifdef USE_API_HOMEASSISTANT_SERVICES
                     public api::CustomAPIDevice,
#endif
                     public hid::HIDInput {
 public:
  void loop() override;
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void setup() override;
#ifdef USE_ESP32_BLE_DEVICE
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;
#endif

  // A slot: the ble_client has no mac_address, pair() finds the device and bt_pairing keeps it.
  void set_slot(uint32_t slot) { this->slot_ = slot; }
  void set_dynamic(bool dynamic) { this->dynamic_ = dynamic; }
  /// A std::string global holding the slot's device address ("" when none).
  void set_address_global(std::string (*get)(), void (*set)(const std::string &)) {
    this->address_get_ = get;
    this->address_set_ = set;
  }
#ifdef USE_BT_PAIRING
  void set_pairing(bt_pairing::BTPairing *pairing) { this->pairing_ = pairing; }
#endif
  /// A slot: look for an advertising HID device for duration_ms, pair the first new one and
  /// keep it. With a configured address: connect, which pairs it if it is not paired yet.
  void pair(uint32_t duration_ms);
  /// Delete the pairing (and forget the device of a slot).
  void unpair();
  bool is_pairing() const { return this->pairing_active_; }

#ifdef USE_TEXT_SENSOR
  void register_last_event_usage_text_sensor(text_sensor::TextSensor *sensor) {
    this->last_event_usage_text_sensor_ = sensor;
  }
  void register_last_event_code_text_sensor(text_sensor::TextSensor *sensor) {
    this->last_event_code_text_sensor_ = sensor;
  }
#endif
#ifdef USE_SENSOR
  void register_last_event_value_sensor(sensor::Sensor *sensor) { this->last_event_value_sensor_ = sensor; }
  void register_battery_sensor(sensor::Sensor *sensor) { this->battery_sensor_ = sensor; }
#endif
  void set_event_sampling_interval_us(uint32_t interval_us) { this->event_sampling_interval_us_ = interval_us; }
  void set_discovery_mode(DiscoveryMode mode) { this->discovery_mode_ = mode; }
  void set_protocol_mode_policy(ProtocolModePolicy policy) { this->protocol_mode_policy_ = policy; }
  /// Fire esphome.hid_events in Home Assistant for every decoded value.
  void set_publish_to_ha(bool publish) { this->publish_to_ha_ = publish; }
  /// 0 disables it. Otherwise a small GATT read on the idle link every interval keeps the
  /// peripheral from going to sleep.
  void set_keepalive_interval(uint32_t interval_ms) { this->keepalive_interval_ms_ = interval_ms; }
  /// Runtime gate for the keepalive; no effect unless keepalive_interval is configured.
  void set_keepalive_enabled(bool enabled);
  bool is_keepalive_enabled() const { return this->keepalive_enabled_; }
  /// Connection events the peripheral may skip while it has nothing to send. Does not delay
  /// key presses; only cuts its idle radio wake-ups.
  void set_conn_latency(uint16_t latency) {
    this->conn_latency_ = latency;
    this->has_conn_latency_ = true;
  }

 protected:
  void write_leds_(uint8_t leds) override;

  void reset_connection_state_();
  void transition_(SetupPhase phase);
  void mark_degraded_(const std::string &reason);
  void handle_phase_timeout_();

  bool inventory_gatt_database_();
  void select_profile_();
  void plan_reads_();
  bool add_read_(const GattServiceInfo &service, const GattCharacteristicInfo &characteristic,
                 const GattDescriptorInfo *descriptor, ReadPurpose purpose, bool required);
  void begin_reads_();
  void start_next_read_();
  void finish_active_read_(esp_gatt_status_t status, const uint8_t *value, size_t length);
  void process_read_value_(const GattReadOperation &operation, const uint8_t *value, size_t length);
  void build_hid_profiles_();
  void log_hid_schema_(const HIDServiceContext &context) const;

  void start_protocol_mode_write_();
  void plan_subscriptions_();
  void start_next_subscription_();
  void handle_registration_result_(uint16_t handle, esp_gatt_status_t status);
  void handle_cccd_result_(uint16_t handle, esp_gatt_status_t status);
  void write_current_cccd_();
  void finalize_setup_();

  bool validate_preferred_conn_params_();
  bool build_desired_conn_params_(esp_ble_conn_update_params_t &out);
  void update_conn_params_();
  void maybe_send_keepalive_();

  uint64_t log_raw_notification_(const esp_ble_gattc_cb_param_t::gattc_notify_evt_param &notification);
  void enqueue_input_report_(const esp_ble_gattc_cb_param_t::gattc_notify_evt_param &notification, uint64_t seq_id);
  void process_input_report_(const PendingHIDReport &report);
  bool events_wanted_() const;
  void process_decoded_values_(const PendingHIDReport &report, const HIDReportSource &source, const std::string &raw,
                               const std::vector<HIDReportItemValue> &values, HIDDecodeStatus status);
  HIDEvent make_hid_event_(const PendingHIDReport &report, const HIDReportSource &source, const std::string &raw,
                           const HIDReportItemValue *value, HIDDecodeStatus status);
  void accumulate_event_(const PendingHIDReport &report, const HIDReportSource &source, const std::string &raw,
                         const HIDReportItemValue &value, HIDDecodeStatus status);
  void update_sampling_window_();
  void flush_sampled_events_(const char *reason);
  void emit_hid_event_(HIDEvent event);

  void request_rediscovery_();
  void restore_paired_();
  void end_pairing_(bool success, const char *reason);
  void schedule_address_(uint64_t address, esp_ble_addr_type_t type, bool auto_connect);
  bool stored_address_(uint8_t *bda, esp_ble_addr_type_t *type) const;

  const GattCharacteristicInfo *find_characteristic_(uint16_t handle) const;
  const GattDescriptorInfo *find_descriptor_(const GattCharacteristicInfo &characteristic, uint16_t uuid) const;
  HIDServiceContext *find_hid_context_(uint16_t service_instance);
  const HIDServiceContext *find_hid_context_(uint16_t service_instance) const;
  static bool uuid16_(const espbt::ESPBTUUID &uuid, uint16_t expected);
  static std::string uuid_string_(const espbt::ESPBTUUID &uuid);
  static std::string sha256_(const uint8_t *data, size_t length);
  static std::string format_value_(const uint8_t *data, size_t length);

  // Per connection: the GATT database as found, and the operations planned on it.
  std::vector<GattServiceInfo> services_;
  std::vector<uint16_t> hid_service_instances_;
  std::deque<GattReadOperation> read_queue_;
  std::unique_ptr<GattReadOperation> active_read_;
  std::deque<SubscriptionInfo> subscription_queue_;
  std::unique_ptr<SubscriptionInfo> active_subscription_;
  std::set<uint16_t> hid_report_handles_;
  std::set<uint16_t> battery_handles_;
  std::set<uint16_t> service_changed_handles_;
  std::set<uint16_t> planned_read_handles_;
  std::deque<PendingHIDReport> pending_reports_;
  size_t pending_report_bytes_{0};
  HIDBootReportDecoder boot_decoder_;
  uint16_t keepalive_handle_{0};

  // Kept across reconnects while the GATT database stays the same (same hash, which covers
  // the handles): the parsed report maps and which report each handle carries.
  std::vector<HIDServiceContext> hid_services_;
  std::map<uint16_t, HIDReportSource> handle_report_source_;
  std::string cached_profile_hash_;
  bool cache_valid_{false};
  bool warm_start_{false};
  bool profile_ready_{false};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *last_event_usage_text_sensor_{nullptr};
  text_sensor::TextSensor *last_event_code_text_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *last_event_value_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
#endif

  SetupPhase setup_phase_{SetupPhase::DISCONNECTED};
  DiscoveryMode discovery_mode_{DiscoveryMode::STANDARD};
  ProtocolModePolicy protocol_mode_policy_{ProtocolModePolicy::UNCHANGED};
  uint32_t phase_started_ms_{0};
  uint32_t operations_succeeded_{0};
  uint32_t operations_failed_{0};
  bool degraded_{false};
  bool congested_{false};
  bool rediscovery_requested_{false};
  bool preferred_conn_params_valid_{false};
  esp_ble_conn_update_params_t preferred_conn_params_{};
  std::deque<uint16_t> protocol_write_handles_;
  SubscriptionAttemptState subscription_state_;
  std::string gatt_profile_hash_;

#ifdef USE_BT_PAIRING
  bt_pairing::BTPairing *pairing_{nullptr};
#endif
  std::string candidate_name_;
  std::string (*address_get_)(){nullptr};
  void (*address_set_)(const std::string &){nullptr};
  uint32_t slot_{0};
  uint32_t pairing_until_{0};
  uint16_t candidate_appearance_{0};
  bool dynamic_{false};
  bool restored_{false};
  bool pairing_active_{false};
  bool candidate_selected_{false};
  uint64_t candidate_address_{0};
  uint64_t pending_address_{0};
  esp_ble_addr_type_t pending_address_type_{BLE_ADDR_TYPE_PUBLIC};
  bool pending_address_valid_{false};
  bool pending_auto_connect_{false};

  bool publish_to_ha_{true};
  uint32_t keepalive_interval_ms_{0};
  uint32_t last_activity_ms_{0};
  bool keepalive_enabled_{true};
  uint16_t conn_latency_{0};
  bool has_conn_latency_{false};

  OrderedEventBuffer sampled_events_{16};
  uint32_t sampling_window_started_us_{0};
  uint32_t event_sampling_interval_us_{0};
  bool sampling_window_active_{false};
  uint32_t last_overflow_warning_{0};
  uint32_t reports_dropped_{0};
  uint64_t next_seq_id_{1};

  static constexpr size_t MAX_SERVICES = 64;
  static constexpr size_t MAX_INCLUDED_SERVICES = 128;
  static constexpr size_t MAX_CHARACTERISTICS = 256;
  static constexpr size_t MAX_DESCRIPTORS = 512;
  static constexpr size_t MAX_READ_OPERATIONS = 384;
  static constexpr size_t MAX_SUBSCRIPTIONS = 128;
  static constexpr size_t MAX_ATTRIBUTE_VALUE_BYTES = 4096;
  static constexpr size_t MAX_PENDING_REPORTS = 64;
  static constexpr size_t MAX_REPORT_PAYLOAD_BYTES = 512;
  static constexpr size_t MAX_PENDING_REPORT_BYTES = 4096;
  static constexpr size_t MAX_REPORTS_PER_LOOP = 8;
  static constexpr uint32_t MAX_PROCESSING_TIME_US = 4000;
  static constexpr uint32_t PHASE_TIMEOUT_MS = 15000;
  static constexpr uint32_t CCCD_FALLBACK_DELAY_MS = 250;
  static constexpr uint32_t OVERFLOW_WARNING_INTERVAL_MS = 5000;
  // The peer applies or rejects a parameter update within a few connection events.
  static constexpr uint32_t CONN_PARAMS_SETTLE_MS = 2000;
};

template<typename... Ts> class PairAction : public Action<Ts...>, public Parented<BLEClientHID> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->pair(this->duration_.value(x...)); }
};

template<typename... Ts> class UnpairAction : public Action<Ts...>, public Parented<BLEClientHID> {
 public:
  void play(const Ts &...x) override { this->parent_->unpair(); }
};

}  // namespace esphome::ble_client_hid

#endif  // USE_ESP32
