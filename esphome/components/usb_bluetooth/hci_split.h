#pragma once

#include "esphome/core/defines.h"
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT

#include <cstddef>
#include <cstdint>

namespace esphome::usb_bluetooth {

class USBBluetooth;

// One Bluedroid, two controllers: the USB dongle for BR/EDR, the co-processor's controller
// (esp_hosted, attached by esp32_ble) for LE. Bluedroid sees a single dual-mode controller:
//
// - Commands go by group: LE (OGF 0x08) and vendor commands (Espressif's) to the LE
//   controller, the rest to the dongle; commands naming a connection go where it lives.
//   Reset, the event masks and the flow control set-up go to both and are answered once.
// - Features come from the dongle with LE marked supported; supported commands are merged.
// - LE connection handles are renumbered (0x0E00 up) so they cannot meet the dongle's.
// - One address for both radios: the co-processor's, written to a Broadcom dongle; any other
//   dongle keeps its own, which then is Bluedroid's.
//
// esp32_ble's esp_bluedroid_attach_hci_driver() is intercepted (linker --wrap): its esp_hosted
// operations become the LE side and Bluedroid is attached to the splitter instead.
namespace hci_split {

/// The dongle is ready: BR/EDR traffic goes to it from now on.
void set_br(USBBluetooth *br);
/// USB task: an H4 packet from the dongle.
void br_recv(uint8_t *h4, size_t len);
/// USB task: the dongle can take another packet.
void br_send_available();
/// Main loop: log ACL continuations that arrived without their start, per side, since the last call.
void report_acl();

}  // namespace hci_split
}  // namespace esphome::usb_bluetooth

#endif  // USE_USB_BLUETOOTH_HCI_SPLIT
