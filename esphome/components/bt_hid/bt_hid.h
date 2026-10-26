#pragma once

#include <atomic>
#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_BT_PAIRING
#include "esphome/components/bt_pairing/bt_pairing.h"
#endif

#include <esp_bt_defs.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace esphome::bt_hid {

// PIN modes for a Wii Remote: paired with 1+2 (its address backwards), or with its sync
// button (this device's address backwards).
static constexpr const char *PIN_WIIMOTE = "wiimote";
static constexpr const char *PIN_WIIMOTE_SYNC = "wiimote_sync";

class BTHIDDeviceSelect;

/// A device the pairing scan found, offered by the select.
struct FoundDevice {
  uint8_t address[6];
  uint32_t device_class;
  std::string name;
};

// One Bluetooth Classic HID device (keyboard, mouse, game controller, remote), through
// Bluedroid's HID host. Its reports feed hid::HIDInput like usb_hid and ble_client_hid. The
// HID host, pairing and connections are shared by all of them; see bt_hid.cpp.
class BTHIDDevice : public Component,
                    public hid::HIDInput
#ifdef USE_BT_PAIRING
    ,
                    public bt_pairing::PairingTarget
#endif
{
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_address(uint64_t address);
  /// With a select, the pairing scan only lists the devices it finds; the one picked is paired.
  void set_device_select(BTHIDDeviceSelect *select) { this->device_select_ = select; }
  bool has_device_select() const { return this->device_select_ != nullptr; }
  const std::vector<FoundDevice> &found_devices() const { return this->found_; }
  static constexpr size_t MAX_FOUND = 12;
  /// Pair with a device the pairing scan listed; opens the window again when it has closed.
  void pair_with(size_t index);
#ifdef USE_BT_PAIRING
  bool pairs(bt_pairing::Transport transport, bt_pairing::DeviceKind kind) const override {
    return transport == bt_pairing::Transport::CLASSIC && kind == bt_pairing::DeviceKind::HID;
  }
  uint32_t pairing_slot() const override { return this->slot_; }
  bool is_free() const override { return !this->fixed_address_ && !this->has_address() && !this->is_pairing(); }
  bool pair_device(const bt_pairing::ScannedDevice &device) override;
  bool is_device_connected() const override { return this->connected_; }
  void connect_device() override { this->connect(); }
  void disconnect_device() override { this->disconnect(); }
  void cancel_device_pairing() override { this->cancel_pairing(); }
  bool holds_many() const override { return this->pooled_; }
  const uint8_t *connected_address() const override {
    return this->pooled_ && this->has_address() ? this->bda_ : nullptr;
  }
  bool is_address_connected(const uint8_t *address) const override {
    return this->connected_ && memcmp(this->bda_, address, ESP_BD_ADDR_LEN) == 0;
  }
  void connect_address(const uint8_t *address) override;
  void disconnect_address(const uint8_t *address) override;
#endif
  /// One of bt_pairing's shared slots: a connection, taken by whichever of its paired devices
  /// connects or is connected to, and given up when that one goes.
  void set_pooled(bool pooled) { this->pooled_ = pooled; }
  bool is_pooled() const { return this->pooled_; }
  uint8_t get_device_kinds() const override {
    return this->connected_ && this->map_ != nullptr ? this->map_->device_kinds() : 0;
  }
  /// Main loop: a device the pairing scan found, or one that connected on its own.
  void on_found(const uint8_t *bda, uint32_t device_class, const std::string &name);
  void set_slot(uint32_t slot) { this->slot_ = slot; }
  /// A std::string global holding the slot's device address ("" when none).
  void set_address_global(std::string (*get)(), void (*set)(const std::string &)) {
    this->address_get_ = get;
    this->address_set_ = set;
  }
#ifdef USE_BT_PAIRING
  void set_pairing(bt_pairing::BTPairing *pairing) { this->pairing_ = pairing; }
#endif
  void set_pin(const std::string &pin) { this->pin_ = pin; }
  void set_auto_connect(bool auto_connect) { this->auto_connect_ = auto_connect; }
  template<typename F> void add_on_passkey_callback(F &&cb) { this->passkey_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_pin_request_callback(F &&cb) { this->pin_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_connect_callback(F &&cb) { this->connect_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_disconnect_callback(F &&cb) { this->disconnect_cb_.add(std::forward<F>(cb)); }

  /// Connect, and keep reconnecting; also lets the device connect on its own again.
  void connect();
  /// Disconnect, and stay disconnected (the device connecting on its own is refused) until connect().
  void disconnect();
  /// Delete the pairing, telling the device first if it is connected.
  void unpair();
  /// Without a configured address: look for a device in pairing mode, pair the first new one
  /// and keep it. With one: connect, which pairs a device that is not paired yet.
  void pair(uint32_t duration_ms);
  /// Look for discoverable Bluetooth Classic HID devices and log them; shared by all devices.
  static void scan(uint32_t duration_ms);
  bool get_auto_connect() const { return this->auto_connect_; }

  bool is_connected() const { return this->connected_; }
  bool is_pairing() const;
  bool has_address() const;
  bool has_fixed_address() const { return this->fixed_address_; }
  const uint8_t *address() const { return this->bda_; }
  const std::string &pin() const { return this->pin_; }

  // Main loop, from the shared host.
  void on_open(uint8_t handle, bool success, int status);
  void on_close();
  void on_descriptor(const std::vector<uint8_t> &descriptor, uint16_t vid, uint16_t pid);
  void on_report(const std::vector<uint8_t> &report);
  void on_feature(const std::vector<uint8_t> &report);
  void on_report_data(const uint8_t *data, size_t length);
  void on_feature_data(const uint8_t *data, size_t length);
  void set_decoding(bool decoding) { this->decoding_.store(decoding, std::memory_order_release); }
  bool decoding() const { return this->decoding_.load(std::memory_order_acquire); }

  // Gamepad driver transport.
  bool send_output_report(const uint8_t *data, size_t len) override;
  bool set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) override;
  bool request_feature_report(uint8_t id, uint16_t len) override;
  Component *hid_component() override { return this; }
  void on_passkey(uint32_t passkey);
  /// The link's signal: the RSSI delta from the controller's golden receive range.
  bool request_signal() override;
  void on_pin_request();
  uint8_t handle() const { return this->handle_; }
  uint32_t next_connect_ms() const { return this->next_connect_ms_; }
  /// Paged once after boot and on connect(); otherwise the device connects on its own.
  bool page_pending() const { return this->page_pending_; }
  void set_page_pending(bool pending) { this->page_pending_ = pending; }
  /// Classic came back: paged once more, at once.
  void page_again();
  void connect_attempted(uint32_t now);
  bool on_pairing_candidate(const uint8_t *bda, uint32_t device_class, const std::string &name);
  void end_pairing(bool success, const char *reason);
  /// Close a running pairing window; the device paired before stays.
  void cancel_pairing();
  /// BTC task, with the host lock held: the device that paired on its own takes the open slot.
  void adopt_address(const uint8_t *bda);
  void on_adopted();
  void restore_paired();
  /// Main loop, shared slots: a paired device took it (it connects), or it is paged for one.
  void on_claimed();
  void hold_for_page(const uint8_t *bda);
  /// Main loop, shared slots: free again.
  void release();
  uint32_t held_since() const { return this->held_since_; }
  /// Main loop, shared slots: the paired Classic HID devices of the shared slot.
  void paired_addresses(std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> &out) const;
#ifdef USE_BT_PAIRING
  bool paired_loaded() const { return this->pairing_ != nullptr && this->pairing_->is_loaded(); }
#endif

 protected:
  void write_leds_(uint8_t leds) override;
  void open_window_(uint32_t until);
  bool pair_found_(const FoundDevice &found);
  void publish_found_();
  void set_bda_(const uint8_t *bda);
  void refresh_paired_();
  bool stored_address_(uint8_t *bda) const;

  esp_bd_addr_t bda_{};
  std::string pin_;
  std::unique_ptr<hid::HIDReportMap> map_;
#ifdef USE_BT_PAIRING
  bt_pairing::BTPairing *pairing_{nullptr};
#endif
  std::string candidate_name_;
  std::vector<FoundDevice> found_;
  BTHIDDeviceSelect *device_select_{nullptr};
  std::string (*address_get_)(){nullptr};
  void (*address_set_)(const std::string &){nullptr};
  CallbackManager<void(uint32_t)> passkey_cb_;
  CallbackManager<void(const std::string &)> pin_cb_;
  CallbackManager<void()> connect_cb_;
  CallbackManager<void()> disconnect_cb_;
  uint32_t next_connect_ms_{0};
  uint32_t retry_ms_{0};
  uint32_t held_since_{0};
  uint32_t slot_{0};
  uint32_t candidate_class_{0};
  uint8_t handle_{0xFF};
  uint8_t feature_id_{0};  // the feature report asked for, 0 none
  bool connected_{false};
  std::atomic<bool> decoding_{false};
  bool auto_connect_{true};
  bool fixed_address_{false};
  bool restored_{false};
  bool page_pending_{true};
  bool pooled_{false};
};

class PasskeyTrigger : public Trigger<uint32_t> {
 public:
  explicit PasskeyTrigger(BTHIDDevice *parent) {
    parent->add_on_passkey_callback([this](uint32_t passkey) { this->trigger(passkey); });
  }
};

template<typename... Ts> class CancelPairingAction : public Action<Ts...> {
 public:
  explicit CancelPairingAction(BTHIDDevice *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->cancel_pairing(); }

 protected:
  BTHIDDevice *parent_;
};

class PinRequestTrigger : public Trigger<std::string> {
 public:
  explicit PinRequestTrigger(BTHIDDevice *parent) {
    parent->add_on_pin_request_callback([this](const std::string &pin) { this->trigger(pin); });
  }
};

class ConnectTrigger : public Trigger<> {
 public:
  explicit ConnectTrigger(BTHIDDevice *parent) {
    parent->add_on_connect_callback([this]() { this->trigger(); });
  }
};

class DisconnectTrigger : public Trigger<> {
 public:
  explicit DisconnectTrigger(BTHIDDevice *parent) {
    parent->add_on_disconnect_callback([this]() { this->trigger(); });
  }
};

template<typename... Ts> class ConnectAction : public Action<Ts...> {
 public:
  explicit ConnectAction(BTHIDDevice *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->connect(); }

 protected:
  BTHIDDevice *parent_;
};

template<typename... Ts> class DisconnectAction : public Action<Ts...> {
 public:
  explicit DisconnectAction(BTHIDDevice *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->disconnect(); }

 protected:
  BTHIDDevice *parent_;
};

template<typename... Ts> class UnpairAction : public Action<Ts...> {
 public:
  explicit UnpairAction(BTHIDDevice *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->unpair(); }

 protected:
  BTHIDDevice *parent_;
};

template<typename... Ts> class PairAction : public Action<Ts...> {
 public:
  explicit PairAction(BTHIDDevice *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->pair(this->duration_.value(x...)); }

 protected:
  BTHIDDevice *parent_;
};

template<typename... Ts> class ScanAction : public Action<Ts...> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { BTHIDDevice::scan(this->duration_.value(x...)); }
};

}  // namespace esphome::bt_hid

#endif  // USE_ESP32
