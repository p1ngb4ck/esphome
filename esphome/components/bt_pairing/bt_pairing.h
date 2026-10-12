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

// The devices paired through bt_hid and ble_client_hid slots, kept across reboots in
// preferences or in a file on a storage. Bond keys stay in Bluedroid's own NVS store;
// forgetting a device also deletes its bond there.
class BTPairing : public Component {
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

  /// Called for every forgotten record, before its bond is deleted.
  template<typename F> void add_on_forget_callback(F &&callback) {
    this->forget_callbacks_.add(std::forward<F>(callback));
  }

 protected:
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
