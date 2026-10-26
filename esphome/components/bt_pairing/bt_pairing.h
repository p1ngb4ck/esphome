#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#ifdef USE_BT_PAIRING_STORAGE
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#endif
#ifdef USE_BT_PAIRING_LE
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#endif
#ifdef USE_BT_CLASSIC
#include "esphome/components/bt_classic/bt_classic.h"
#endif
#ifdef USE_BT_PAIRING_HID
#include "esphome/components/hid/hid_input.h"
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

// BROADCAST: a BLE device that only advertises (buttons, remotes), heard without a connection.
enum class DeviceKind : uint8_t { HID = 0, AUDIO = 1, BROADCAST = 2 };

#ifdef USE_BT_PAIRING_LE
/// FNV-1a over the manufacturer and service data of an advertisement.
uint32_t advertisement_hash(const ble_device_base::ESPBTDevice &device);
#endif

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
  virtual bool is_device_connected() const = 0;
  /// Connect to its device, and keep reconnecting.
  virtual void connect_device() = 0;
  /// Disconnect from its device, and stay disconnected until connect_device().
  virtual void disconnect_device() = 0;
  /// Stop a pairing it is running; the device paired before stays.
  virtual void cancel_device_pairing() {}
  /// Its slot keeps several devices (one connected at a time); else a new one replaces the old.
  virtual bool holds_many() const { return false; }
  /// The device of its slot connected now (holds_many), else nullptr.
  virtual const uint8_t *connected_address() const { return nullptr; }
  /// One of its devices: connected, connect to it (the one connected first goes), disconnect it.
  virtual bool is_address_connected(const uint8_t *address) const { return this->is_device_connected(); }
  virtual void connect_address(const uint8_t *address) { this->connect_device(); }
  virtual void disconnect_address(const uint8_t *address) { this->disconnect_device(); }
};

class BTPairingSelect;

// The devices paired through bt_hid and ble_client_hid slots, kept across reboots in
// preferences or in a file on a storage. Bond keys stay in Bluedroid's own NVS store;
// forgetting a device also deletes its bond there.
// With max_hid_devices, it is also the HID input of every HID device paired through it: keys,
// typed text (in its layout), pointer and controls, each from the device event_device() names.
class BTPairing : public Component
#ifdef USE_BT_PAIRING_LE
    ,
                  public esp32_ble_tracker::ESPBTDeviceListener
#endif
#ifdef USE_BT_PAIRING_HID
    ,
                  public hid::HIDInput
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
  /// HID slots in use at most (Bluetooth Classic: connected devices, BLE: paired ones); 0: no limit.
  void set_max_hid_devices(uint8_t max) { this->max_hid_devices_ = max; }
#ifdef USE_BT_PAIRING_HID
  /// A slot's HID input, forwarded as this one's.
  void add_hid_slot(hid::HIDInput *input, PairingTarget *target);
  /// Main loop: what the connected slots' devices are.
  uint8_t get_device_kinds() const override;
  /// The device the HID event being handled came from: its name, else its address.
  const std::string &event_device() const { return this->event_device_; }
  /// The paired devices of its HID slots ("bt:AA:BB:CC:DD:EE:FF"); each types with its own layout.
  void list_devices(std::vector<hid::InputDevice> &out) const override;
  /// HIDDeviceKind bits a paired device announced: Classic in its Class of Device, LE in its appearance.
  static uint8_t record_kinds(const PairingRecord &record);
  void set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) override;
  /// The HID slot the device (bt:MAC) is connected to.
  hid::HIDInput *device_input(const std::string &key) override;
  /// Pad outputs go to every slot's device.
  void rumble(uint8_t strong, uint8_t weak, uint32_t duration_ms) override {
    hid::HIDInput::rumble(strong, weak, 0);
    for (auto *input : this->hid_slots_)
      input->rumble(strong, weak, duration_ms);
  }
  void set_player_led(uint8_t player) override {
    hid::HIDInput::set_player_led(player);
    for (auto *input : this->hid_slots_)
      input->set_player_led(player);
  }
  void set_light_bar(uint8_t red, uint8_t green, uint8_t blue) override {
    hid::HIDInput::set_light_bar(red, green, blue);
    for (auto *input : this->hid_slots_)
      input->set_light_bar(red, green, blue);
  }
#endif
  /// A slot's device: its name, else its address; "" when it has none.
  std::string device_label(const PairingTarget *target) const;

  // From the slots, main loop.
  void on_pin_request(const PairingTarget *target, const std::string &pin);
  void on_passkey(const PairingTarget *target, uint32_t passkey);
  void on_connection(const PairingTarget *target, bool connected);

  /// By address, "AA:BB:CC:DD:EE:FF". False when no listed / paired device has it.
  bool pair_address(const std::string &address);
  bool connect(const std::string &address);
  bool disconnect(const std::string &address);
  bool unpair(const std::string &address);
  bool is_connected(const std::string &address) const;
  /// (device, connected) whenever a paired device connects or disconnects.
  template<typename F> void add_on_connection_callback(F &&callback) {
    this->connection_callbacks_.add(std::forward<F>(callback));
  }
  /// A configured slot owns this slot id; a record of any other id is left from an old config.
  bool slot_in_use(uint32_t slot) const;
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
  /// Main loop: the scan's list changed (a device found or named), while the scan still runs too.
  template<typename F> void add_on_scanned_callback(F &&callback) {
    this->scanned_callbacks_.add(std::forward<F>(callback));
  }
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
  PairingTarget *target_for_(const ScannedDevice &device, const char **reason) const;
  PairingTarget *target_of_(const uint8_t *address) const;
  void migrate_records_();
  bool slot_holds_many_(uint32_t slot) const;
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
  void device_label_into_(const PairingTarget *target, std::string &out) const;
#ifdef USE_BT_PAIRING_STORAGE
  storage::PathStorage *resolve_file_(const char **rel);
  // The file, through the storage worker (the main loop never waits for it): read once, written
  // anew (a .tmp moved over it) after each change.
  void file_load_();
  void file_read_next_();
  void file_loaded_(LoadResult result);
  void file_save_();
  void file_failed_(const char *what, storage::StorageError err);
  size_t encode_into_(uint8_t *out) const;
#endif

  std::vector<PairingRecord> records_;
  CallbackManager<void(const PairingRecord &)> forget_callbacks_;
  CallbackManager<void(const std::string &)> status_callbacks_;
  CallbackManager<void()> scanned_callbacks_;
  CallbackManager<void(const std::string &, bool)> connection_callbacks_;
#ifdef USE_BT_PAIRING_HID
  std::string event_device_;
#endif
  ScannedDevice dispatched_{};
  const PairingTarget *dispatched_target_{nullptr};
  uint8_t max_hid_devices_{0};
  std::vector<PairingTarget *> targets_;
#ifdef USE_BT_PAIRING_HID
  std::vector<hid::HIDInput *> hid_slots_;
  std::vector<PairingTarget *> hid_targets_;  // the slots' targets, as hid_slots_
  struct DeviceLayout {
    uint8_t address[6];
    const hid::KeyboardLayout *layout;
  };
  std::vector<DeviceLayout> device_layouts_;
  /// The layout of the device a slot holds now; null: this input's own.
  const hid::KeyboardLayout *slot_layout_(const PairingTarget *target) const;
  void devices_changed_() { this->devices_cb_.call(); }
#endif
  std::vector<ScannedDevice> scanned_;
  BTPairingSelect *device_select_{nullptr};
  Mutex found_lock_;
  std::deque<ScannedDevice> found_;  // Classic results from the BTC task
  ScannedDevice pending_{};
  PairingTarget *pending_target_{nullptr};
  uint32_t scan_until_{0};
  std::vector<std::pair<uint64_t, uint32_t>> adv_hashes_;  // first payload heard per address
  bool list_broadcast_{false};
  std::atomic<bool> scanning_{false};
  std::atomic<bool> classic_discovering_{false};
  ESPPreferenceObject pref_;
  uint32_t preference_key_{0};
  uint32_t next_attempt_ms_{0};
#ifdef USE_BT_PAIRING_STORAGE
  const char *file_{nullptr};
  uint8_t *io_buf_{nullptr};  // PSRAM, a whole database
  size_t io_len_{0};
  size_t io_got_{0};
  storage::PathStorage *io_storage_{nullptr};
  const char *io_rel_{nullptr};
  std::string io_tmp_;
  storage::StreamHandle io_handle_{};
  bool io_busy_{false};
  uint32_t change_gen_{0};  // counts the changes; the one a write took
  uint32_t save_gen_{0};
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

class ConnectionTrigger : public Trigger<std::string> {
 public:
  ConnectionTrigger(BTPairing *parent, bool connected) {
    parent->add_on_connection_callback([this, connected](const std::string &device, bool state) {
      if (state == connected)
        this->trigger(device);
    });
  }
};

template<typename... Ts> class PairAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  TEMPLATABLE_VALUE(std::string, address)
  void play(const Ts &...x) override { this->parent_->pair_address(this->address_.value(x...)); }
};

template<typename... Ts> class ConnectAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  TEMPLATABLE_VALUE(std::string, address)
  void play(const Ts &...x) override { this->parent_->connect(this->address_.value(x...)); }
};

template<typename... Ts> class DisconnectAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  TEMPLATABLE_VALUE(std::string, address)
  void play(const Ts &...x) override { this->parent_->disconnect(this->address_.value(x...)); }
};

template<typename... Ts> class UnpairAction : public Action<Ts...>, public Parented<BTPairing> {
 public:
  TEMPLATABLE_VALUE(std::string, address)
  void play(const Ts &...x) override { this->parent_->unpair(this->address_.value(x...)); }
};

#ifdef USE_BT_PAIRING_HID
/// on_key: (device, key, pressed); with a key set, only that one.
class DeviceKeyTrigger : public Trigger<std::string, std::string, bool> {
 public:
  explicit DeviceKeyTrigger(BTPairing *parent) {
    parent->add_on_key_event_callback([this, parent](uint16_t page, uint16_t usage, bool pressed) {
      if (this->any_ || (page == this->page_ && usage == this->usage_))
        this->trigger(parent->event_device(), hid::key_name(page, usage), pressed);
    });
  }
  void set_key(uint16_t page, uint16_t usage) {
    this->page_ = page;
    this->usage_ = usage;
    this->any_ = false;
  }

 protected:
  uint16_t page_{0};
  uint16_t usage_{0};
  bool any_{true};
};

class DeviceTextTrigger : public Trigger<std::string, std::string> {
 public:
  explicit DeviceTextTrigger(BTPairing *parent) {
    parent->add_on_text_callback(
        [this, parent](const std::string &text) { this->trigger(parent->event_device(), text); });
  }
};

class DevicePointerTrigger : public Trigger<std::string, int, int, int, int, int> {
 public:
  explicit DevicePointerTrigger(BTPairing *parent) {
    parent->add_on_pointer_callback(
        [this, parent](int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
          this->trigger(parent->event_device(), dx, dy, wheel, pan, buttons);
        });
  }
};

class DeviceControlTrigger : public Trigger<std::string, std::string, int> {
 public:
  explicit DeviceControlTrigger(BTPairing *parent) {
    parent->add_on_control_callback([this, parent](uint16_t page, uint16_t usage, int32_t value) {
      this->trigger(parent->event_device(), hid::key_name(page, usage), value);
    });
  }
};
#endif

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
