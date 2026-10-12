#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <esp_bt_defs.h>

#include <memory>
#include <string>
#include <vector>

namespace esphome::bt_hid {

// One Bluetooth Classic HID device (keyboard, mouse, game controller, remote), through
// Bluedroid's HID host. Its reports feed hid::HIDInput like usb_hid and ble_client_hid. The
// HID host, pairing and connections are shared by all of them; see bt_hid.cpp.
class BTHIDDevice : public Component, public hid::HIDInput {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_address(uint64_t address);
  void set_pin(const std::string &pin) { this->pin_ = pin; }
  template<typename F> void add_on_passkey_callback(F &&cb) { this->passkey_cb_.add(std::forward<F>(cb)); }

  bool is_connected() const { return this->connected_; }
  const uint8_t *address() const { return this->bda_; }
  const std::string &pin() const { return this->pin_; }

  // Main loop, from the shared host.
  void on_open(uint8_t handle, bool success, int status);
  void on_close();
  void on_descriptor(const std::vector<uint8_t> &descriptor);
  void on_report(const std::vector<uint8_t> &report);
  void on_passkey(uint32_t passkey);
  uint8_t handle() const { return this->handle_; }
  uint32_t next_connect_ms() const { return this->next_connect_ms_; }
  void connect_attempted(uint32_t now);

 protected:
  void write_leds_(uint8_t leds) override;

  esp_bd_addr_t bda_{};
  std::string pin_;
  std::unique_ptr<hid::HIDReportMap> map_;
  CallbackManager<void(uint32_t)> passkey_cb_;
  uint32_t next_connect_ms_{0};
  uint32_t retry_ms_{0};
  uint8_t handle_{0xFF};
  bool connected_{false};
};

class PasskeyTrigger : public Trigger<uint32_t> {
 public:
  explicit PasskeyTrigger(BTHIDDevice *parent) {
    parent->add_on_passkey_callback([this](uint32_t passkey) { this->trigger(passkey); });
  }
};

}  // namespace esphome::bt_hid

#endif  // USE_ESP32
