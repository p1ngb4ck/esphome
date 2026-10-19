#pragma once

#include "esphome/components/hid/gamepad.h"
#include "esphome/components/hid/hid_input.h"

#include <cstdint>

namespace esphome::usb_hid {

class USBHIDDevice;

// Windows Media Center eHome IR receivers (Linux mceusb.c), which are not HID: raw IR samples,
// decoded as RC6 (ir-rc6-decoder.c), MCE remote codes reported as keys (rc-rc6-mce.c).
class MCEReceiverDriver : public hid::GamepadDriver {
 public:
  enum Flags : uint8_t { GEN1 = 1 << 0, GEN3 = 1 << 1, INTERFACE_2 = 1 << 2, KNOWN = 1 << 7 };
  /// Flags of a receiver by vendor/product; 0 for none.
  static uint8_t model(uint16_t vid, uint16_t pid);
  /// Every receiver, for the USB matches.
  template<typename F> static void for_each(F &&f);

  MCEReceiverDriver(USBHIDDevice *client, uint8_t flags);
  ~MCEReceiverDriver() override;
  const char *name() const override { return "MCE IR receiver"; }
  void start() override;
  bool report(const uint8_t *data, size_t len) override;

 protected:
  enum Parser : uint8_t { CMD_HEADER, SUBCMD, CMD_DATA, PARSE_IRDATA };
  enum Rc6 : uint8_t {
    INACTIVE,
    PREFIX_SPACE,
    HEADER_BIT_START,
    HEADER_BIT_END,
    TOGGLE_START,
    TOGGLE_END,
    BODY_BIT_START,
    BODY_BIT_END,
    FINISHED,
  };

  void command_(const uint8_t *cmd, size_t len);
  void sample_(bool pulse, uint32_t us);
  void timeout_();
  void rc6_(bool pulse, uint32_t duration);
  void scancode_(uint32_t scancode, bool toggle);
  void release_();

  USBHIDDevice *client_;
  uint8_t flags_;
  // mceusb parser
  Parser parser_{CMD_HEADER};
  uint8_t cmd_{0};
  uint8_t rem_{0};
  // samples of one polarity, summed until it changes
  bool acc_pulse_{false};
  bool acc_valid_{false};
  uint32_t acc_us_{0};
  // RC6
  Rc6 state_{INACTIVE};
  uint8_t header_{0};
  bool toggle_{false};
  uint8_t count_{0};
  uint8_t wanted_bits_{0};
  uint64_t body_{0};
  // the key held: repeats keep it, a new press (toggle flips) or a pause ends it
  bool held_{false};
  uint32_t held_scancode_{0};
  bool held_toggle_{false};
  uint16_t held_page_{0};
  uint16_t held_usage_{0};
};

struct MCEReceiverModel {
  uint16_t vid, pid;
  uint8_t flags;
};
extern const MCEReceiverModel MCE_RECEIVERS[];
extern const size_t MCE_RECEIVER_COUNT;

template<typename F> void MCEReceiverDriver::for_each(F &&f) {
  for (size_t i = 0; i < MCE_RECEIVER_COUNT; i++)
    f(MCE_RECEIVERS[i]);
}

}  // namespace esphome::usb_hid
