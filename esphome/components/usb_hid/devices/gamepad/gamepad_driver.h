#pragma once

#include "esphome/components/usb_hid/usb_hid.h"

namespace esphome {
namespace usb_hid {

// Generic HID gamepad driver — matches protocol 0x00 (none/vendor) on non-Xbox devices.
// Reports 8 bytes: [report_id, buttons_lo, buttons_hi, hat, lx, ly, rx, ry]
// (or similar vendor formats — this is a best-effort generic decoder).

class GamepadDriver : public HIDDeviceDriver {
 public:
  explicit GamepadDriver(USBHIDClient *parent) : parent_(parent) {}

  bool match_device(uint8_t protocol, uint16_t vid, uint16_t pid) override {
    // Skip Xbox controllers (handled separately)
    if (vid == XBOX360_VID && (pid == XBOX360_PID_WIRED || pid == XBOX360_PID_WIRELESS))
      return false;
    if (vid == BITDO_VID && pid == BITDO_PID_XBOX)
      return false;
    // Generic: match any non-keyboard, non-mouse HID device
    return protocol == HID_PROTO_NONE;
  }

  void process_report(const uint8_t *data, size_t len, HIDDevice * /*device*/) override {
    // Generic format: at least 3 bytes [buttons_lo, buttons_hi, hat/extra]
    if (len < 3)
      return;
    ESP_LOGD("GamepadDriver", "Report [%02X %02X %02X%s]",
             data[0], data[1], data[2], len > 3 ? "..." : "");
  }

  const char *get_name() override { return "Gamepad"; }

 protected:
  USBHIDClient *parent_;
};

}  // namespace usb_hid
}  // namespace esphome
