#pragma once

#include "esphome/components/hid/gamepad.h"
#include "esphome/components/hid/hid_input.h"

#include <cstdint>

namespace esphome::usb_hid {

// Xbox 360 pads, wired and on the wireless receiver (Linux xpad.c, xpad360_process_packet()).
class Xbox360Driver : public hid::GamepadDriver {
 public:
  Xbox360Driver(hid::HIDInput *input, bool wireless) : GamepadDriver(input), wireless_(wireless) {}
  const char *name() const override { return this->wireless_ ? "Xbox 360 Wireless" : "Xbox 360"; }
  void start() override;
  bool report(const uint8_t *data, size_t len) override;
  void outputs(const hid::GamepadOutputs &out) override;

 protected:
  void leds_(uint8_t player);

  bool wireless_;
  bool present_{false};
  uint8_t player_{0xFF};
};

// Xbox One and Series pads, GIP (Linux xpad.c, xpadone_process_packet() and its init packets).
class XboxOneDriver : public hid::GamepadDriver {
 public:
  XboxOneDriver(hid::HIDInput *input, uint16_t vid, uint16_t pid, uint16_t bcd);
  const char *name() const override { return "Xbox One"; }
  void start() override;
  bool report(const uint8_t *data, size_t len) override;
  void outputs(const hid::GamepadOutputs &out) override;

 protected:
  void init_();
  void send_(uint8_t *packet, size_t len);
  void publish_();

  uint16_t vid_;
  uint16_t pid_;
  uint16_t bcd_;
  uint8_t share_offset_{0};  // from the end of the input packet; 0 none
  bool delay_init_{false};
  bool initialized_{false};
  bool guide_{false};
  uint8_t paddles_{0};  // from the firmware packet of the Elite 2 with 5.11 firmware
  uint8_t seq_{0};
  hid::GamepadState state_;
};

}  // namespace esphome::usb_hid
