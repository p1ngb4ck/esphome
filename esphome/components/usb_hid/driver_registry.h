#pragma once

// Include only drivers enabled via build flags set in __init__.py

#ifdef USB_HID_ENABLE_KEYBOARD
#include "devices/keyboard/keyboard_driver.h"
#define HAS_KEYBOARD_DRIVER
#endif

#ifdef USB_HID_ENABLE_MOUSE
#include "devices/mouse/mouse_driver.h"
#define HAS_MOUSE_DRIVER
#endif

#ifdef USB_HID_ENABLE_GAMEPAD
#include "devices/gamepad/gamepad_driver.h"
#define HAS_GAMEPAD_DRIVER
#endif

namespace esphome {
namespace usb_hid {

class USBHIDClient;  // forward declaration

inline void register_all_drivers(USBHIDClient *component) {
#ifdef HAS_KEYBOARD_DRIVER
  component->register_device_driver(new KeyboardDriver(component));
#endif

#ifdef HAS_MOUSE_DRIVER
  component->register_device_driver(new MouseDriver(component));
#endif

  // Generic gamepad registered last (fallback)
#ifdef HAS_GAMEPAD_DRIVER
  component->register_device_driver(new GamepadDriver(component));
#endif
}

}  // namespace usb_hid
}  // namespace esphome
