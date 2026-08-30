#pragma once

// Every driver header sits next to this one rather than under devices/, because
// ComponentManifest.resources() in esphome/loader.py lists only the files directly in a
// component's directory. Subdirectories are descended into only for manifests built with
// recursive_sources=True, which is set for esphome.core.config alone -- so a header under
// devices/ is never copied into the build tree and does not exist at compile time, however
// correct the include path looks. The Python packages under devices/ are unaffected: they
// are imported, not compiled.

// Include only drivers enabled via build flags set in __init__.py

#ifdef USB_HID_ENABLE_KEYBOARD
#include "keyboard_driver.h"
#define HAS_KEYBOARD_DRIVER
#endif

#ifdef USB_HID_ENABLE_MOUSE
#include "mouse_driver.h"
#define HAS_MOUSE_DRIVER
#endif

#ifdef USB_HID_ENABLE_GAMEPAD
#include "gamepad_driver.h"
#define HAS_GAMEPAD_DRIVER
#endif

#ifdef USB_HID_ENABLE_RAW
#include "raw_driver.h"
#define HAS_RAW_DRIVER
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

  // The raw driver matches everything, so it is registered after the ones that identify a
  // device by its protocol. Which device reaches it at all is decided by the vid and pid on
  // the client, and codegen only builds it in where a raw section asked for it.
#ifdef HAS_RAW_DRIVER
  component->register_raw_driver(new RawHIDDriver(component));
#endif
}

}  // namespace usb_hid
}  // namespace esphome
