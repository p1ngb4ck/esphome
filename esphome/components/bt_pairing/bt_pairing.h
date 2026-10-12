#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#ifdef USE_BT_PAIRING_STORAGE
#include "esphome/components/storage/storage.h"
#endif
#ifdef USE_BT_PAIRING_LE
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#endif
#ifdef USE_BT_CLASSIC
#include "esphome/components/bt_classic/bt_classic.h"
#endif

#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace esphome::bt_pairing {

enum class Transport : uint8_t { CLASSIC = 0, LE = 1 };

// One paired device, in the layout the preferences blob and the file hold.
struct PairingRecord {
  uint8_t address[6];
  uint8_t transport;      // Transport
  uint8_t address_type;   // LE: esp_ble_addr_type_t
  uint32_t slot;          // fnv1 hash of the owning bt_hid / ble_client_hid id
  uint32_t device_class;  // Classic: Class of Device; LE: appearance; 0 if unknown
  char name[32];          // NUL-terminated
};
static_assert(sizeof(PairingRecord) == 48, "PairingRecord is a storage format");

enum class DeviceKind : uint8_t { HID = 0, AUDIO = 1 };

/// A device the pairing scan found.
struct ScannedDevice {
  uint8_t address[6];
  Transport transport;
  DeviceKind kind;
  uint8_t address_type;   // LE: esp_ble_addr_type_t
  uint32_t device_class;  // Classic: Class of Device; LE: appearance; 0 if unknown
  std::string name;
};

/// What a device picked from the pairing scan is handed to: bt_hid and ble_client_hid slots,
/// a2dp_source.
class PairingTarget {
 public:
  virtual ~PairingTarget() = default;
  virtual bool pairs(Transport transport, DeviceKind kind) const = 0;
  /// The slot its pairing records carry; 0 when it keeps none.
  virtual uint32_t pairing_slot() const = 0;
  /// It can take a new device without replacing one.
  virtual bool is_free() const = 0;
  /// Main loop. False when it cannot pair now.
  virtual bool pair_device(const ScannedDevice &device) = 0;
};

class BTPairingSelect;

// The devices paired through bt_hid and ble_client_hid slots, kept across reboots in
// preferences or in a file on a storage. Bond keys stay in Bluedroid's own NVS store;
// forgetting a device also deletes its bond there.
class BTPairing : public Component
#ifdef USE_BT_PAIRING_LE
    ,
                  public esp32_ble_tracker::ESPBTDeviceListener
#endif
{
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_max_devices(uint8_t max_devices) { this->max_devices_ = max_devices; }
  void set_preference_key(uint32_t key) { this->preference_key_ = key; }
#ifdef USE_BT_PAIRING_STORAGE
  void set_file(const char *file) { this->file_ = file; }
#endif

  /// False until the database has been read; slots wait for it before connecting.
  bool is_loaded() const { return this->loaded_; }
  const std::vector<PairingRecord> &records() const { return this->records_; }
  const PairingRecord *find_slot(uint32_t slot) const;
  const PairingRecord *find_address(const uint8_t *address) const;

  /// Adds the record, replacing the one of the same slot or address. False when full.
  bool store(const PairingRecord &record);
  void forget_slot(uint32_t slot);
  bool forget_address(const uint8_t *address);
  void forget_all();
  void log_records() const;
  /// Deletes Bluedroid's bond with the device, without touching the database.
  static void delete_bond(const uint8_t *address, Transport transport);

  /// Called for every forgotten record, before its bond is deleted.
  template<typename F> void add_on_forget_callback(F &&callback) {
    this->forget_callbacks_.add(std::forward<F>(callback));
  }

  void add_target(PairingTarget *target) { this->targets_.push_back(target); }
  void set_device_select(BTPairingSelect *select) { this->device_select_ = select; }
  static constexpr size_t MAX_SCANNED = 16;
  /// Lists the Bluetooth Classic and LE devices that can be paired, for duration_ms.
  void scan(uint32_t duration_ms);
  /// Stops the scan and drops a pick waiting for it to stop.
  void cancel_scan();
  bool is_scanning() const { return this->scanning_.load(); }
  const std::vector<ScannedDevice> &scanned_devices() const { return this->scanned_; }
  /// Pairs a listed device through the slot or source that takes its kind.
  void pair_with(size_t index);
  /// What the scan and a pick do, for the log, a screen or Home Assistant.
  template<typename F> void add_on_status_callback(F &&callback) {
    this->status_callbacks_.add(std::forward<F>(callback));
  }
#ifdef USE_BT_PAIRING_LE
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;
#endif

 protected:
  void status_(const std::string &message);
  void add_scanned_(const ScannedDevice &device);
  void publish_scanned_();
  PairingTarget *target_for_(const ScannedDevice &device) const;
  void dispatch_(const ScannedDevice &device, PairingTarget *target);
  void scan_loop_();
#ifdef USE_BT_CLASSIC
  static bool gap_handler_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg);
#endif

  enum class LoadResult : uint8_t { OK, EMPTY, RETRY };
  LoadResult load_();
  bool save_();
  bool decode_(const uint8_t *data, size_t length);
  std::vector<uint8_t> encode_(bool fixed_size) const;
  void forget_index_(size_t index);
#ifdef USE_BT_PAIRING_STORAGE
  storage::PathStorage *resolve_file_(const char **rel);
#endif

  std::vector<PairingRecord> records_;
  CallbackManager<void(const PairingRecord &)> forget_callbacks_;
  CallbackManager<void(const std::string &)> status_callbacks_;
  std::vector<PairingTarget *> targets_;
  std::vector<ScannedDevice> scanned_;
  BTPairingSelect *device_select_{nullptr};
  Mutex found_lock_;
  std::deque<ScannedDevice> found_;  // Classic results from the BTC task
  ScannedDevice pending_{};
  PairingTarget *pending_target_{nullptr};
  uint32_t scan_until_{0};
  std::atomic<bool> scanning_{false};
  std::atomic<bool> classic_discovering_{false};
  bool gap_registered_{false};
  ESPPreferenceObject pref_;
  uint32_t preference_key_{0};
  uint32_t next_attempt_ms_{0};
#ifdef USE_BT_PAIRING_STORAGE
  const char *file_{nullptr};
#endif
  uint8_t max_devices_{8};
  bool loaded_{false};
  bool dirty_{false};
};

template<typename... Ts> class ForgetAllAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  void play(const Ts &...x) override { this->parent_->forget_all(); }
};

template<typename... Ts> class ForgetAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  TEMPLATABLE_VALUE(std::string, address)
  void play(const Ts &...x) override;
};

template<typename... Ts> class ScanAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->scan(this->duration_.value(x...)); }
};

template<typename... Ts> class CancelScanAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  void play(const Ts &...x) override { this->parent_->cancel_scan(); }
};

class StatusTrigger : public Trigger<std::string> {
 public:
  explicit StatusTrigger(BTPairing *parent) {
    parent->add_on_status_callback([this](const std::string &message) { this->trigger(message); });
  }
};

template<typename... Ts> class LogAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  void play(const Ts &...x) override { this->parent_->log_records(); }
};

bool parse_address(const std::string &text, uint8_t *address);

template<typename... Ts> void ForgetAction<Ts...>::play(const Ts &...x) {
  uint8_t address[6];
  if (parse_address(this->address_.value(x...), address))
    this->parent_->forget_address(address);
}

}  // namespace esphome::bt_pairing

#endif  // USE_ESP32
