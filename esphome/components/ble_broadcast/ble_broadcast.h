#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/bt_pairing/bt_pairing.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <string>
#include <vector>

namespace esphome::ble_broadcast {

/// A BLE device that only advertises (a button, a remote): picked in the bt_pairing scan, then
/// heard without a connection. Every change of its advertised data fires the triggers with the
/// data, for a lambda to decode.
class BLEBroadcast : public Component, public esp32_ble_tracker::ESPBTDeviceListener, public bt_pairing::PairingTarget {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_pairing(bt_pairing::BTPairing *pairing) { this->pairing_ = pairing; }
  void set_slot(uint32_t slot) { this->slot_ = slot; }
  /// A std::string global holding the slot's device address ("" when none).
  void set_address_global(std::string (*get)(), void (*set)(const std::string &)) {
    this->address_get_ = get;
    this->address_set_ = set;
  }

  bool parse_device(const ble_device_base::ESPBTDevice &device) override;

  bool pairs(bt_pairing::Transport transport, bt_pairing::DeviceKind kind) const override {
    return transport == bt_pairing::Transport::LE && kind == bt_pairing::DeviceKind::BROADCAST;
  }
  uint32_t pairing_slot() const override { return this->slot_; }
  bool is_free() const override { return this->address_ == 0; }
  bool pair_device(const bt_pairing::ScannedDevice &device) override;
  /// Listening counts as connected; there is no link to make or drop.
  bool is_device_connected() const override { return this->address_ != 0; }
  void connect_device() override {}
  void disconnect_device() override {}
  void unpair();
  /// The device listened to, 0 when none.
  uint64_t get_address() const { return this->address_; }

  /// (data, company id) for every manufacturer data element, when the advertisement changed.
  template<typename F> void add_on_manufacturer_data_callback(F &&cb) {
    this->manufacturer_cb_.add(std::forward<F>(cb));
  }
  /// (data, 16-bit service UUID, 0 for a longer one) for every service data element.
  template<typename F> void add_on_service_data_callback(F &&cb) { this->service_cb_.add(std::forward<F>(cb)); }

 protected:
  void set_address_(const uint8_t *bda);

  bt_pairing::BTPairing *pairing_{nullptr};
  std::string (*address_get_)(){nullptr};
  void (*address_set_)(const std::string &){nullptr};
  CallbackManager<void(const std::vector<uint8_t> &, uint16_t)> manufacturer_cb_;
  CallbackManager<void(const std::vector<uint8_t> &, uint16_t)> service_cb_;
  uint64_t address_{0};
  uint32_t slot_{0};
  uint32_t last_hash_{0};
};

class ManufacturerDataTrigger : public Trigger<std::vector<uint8_t>, uint16_t> {
 public:
  explicit ManufacturerDataTrigger(BLEBroadcast *parent) {
    parent->add_on_manufacturer_data_callback(
        [this](const std::vector<uint8_t> &data, uint16_t id) { this->trigger(data, id); });
  }
};

class ServiceDataTrigger : public Trigger<std::vector<uint8_t>, uint16_t> {
 public:
  explicit ServiceDataTrigger(BLEBroadcast *parent) {
    parent->add_on_service_data_callback(
        [this](const std::vector<uint8_t> &data, uint16_t uuid) { this->trigger(data, uuid); });
  }
};

template<typename... Ts> class UnpairAction : public Action<Ts...>, public Parented<BLEBroadcast> {
 public:
  void play(const Ts &...x) override { this->parent_->unpair(); }
};

}  // namespace esphome::ble_broadcast

#endif  // USE_ESP32
