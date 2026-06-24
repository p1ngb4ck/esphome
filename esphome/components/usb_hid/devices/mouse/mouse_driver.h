#pragma once

#include "esphome/components/usb_hid/usb_hid.h"

namespace esphome {
namespace usb_hid {

class MouseDriver : public HIDDeviceDriver {
 public:
  explicit MouseDriver(USBHIDClient *parent) : parent_(parent) {}

  bool match_device(uint8_t protocol, uint16_t /*vid*/, uint16_t /*pid*/) override {
    return protocol == HID_PROTO_MOUSE;
  }

  void process_report(const uint8_t *data, size_t len, HIDDevice * /*device*/) override {
    if (len < 3)
      return;

    uint8_t buttons = data[0];
    int8_t x        = static_cast<int8_t>(data[1]);
    int8_t y        = static_cast<int8_t>(data[2]);
    int8_t wheel    = (len >= 4) ? static_cast<int8_t>(data[3]) : 0;

#ifdef USE_BINARY_SENSOR
    if (this->parent_->get_mouse_left_sensor())
      this->parent_->get_mouse_left_sensor()->publish_state(buttons & 0x01);
    if (this->parent_->get_mouse_right_sensor())
      this->parent_->get_mouse_right_sensor()->publish_state(buttons & 0x02);
    if (this->parent_->get_mouse_middle_sensor())
      this->parent_->get_mouse_middle_sensor()->publish_state(buttons & 0x04);
#endif

#ifdef USE_SENSOR
    if (x != 0 && this->parent_->get_mouse_x_sensor())
      this->parent_->get_mouse_x_sensor()->publish_state(x);
    if (y != 0 && this->parent_->get_mouse_y_sensor())
      this->parent_->get_mouse_y_sensor()->publish_state(y);
    if (wheel != 0 && this->parent_->get_mouse_wheel_sensor())
      this->parent_->get_mouse_wheel_sensor()->publish_state(wheel);
#endif
  }

  const char *get_name() override { return "Mouse"; }

 protected:
  USBHIDClient *parent_;
};

}  // namespace usb_hid
}  // namespace esphome
