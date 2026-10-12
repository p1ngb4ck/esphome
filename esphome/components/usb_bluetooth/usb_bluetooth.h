#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace esphome::usb_bluetooth {

// A USB Bluetooth dongle as a usb_host client (Bluetooth Core Specification, USB transport):
// HCI commands as class requests on endpoint 0, events on the interrupt IN endpoint, ACL data
// on the bulk pair of the controller interface (E0/01/01, or FF/01/01 with vid/pid).
//
// Reads are one wMaxPacketSize packet each and reassembled by the packet's own length field:
// an event or ACL packet whose length is a whole number of USB packets ends without a short
// packet, so a larger read would wait for data that is not coming.
//
// On connect the controller is reset and asked for its version, address and name (logged).
class USBBluetooth : public usb_host::USBClient {
 public:
  USBBluetooth(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {
    if (vid == 0 && pid == 0)
      this->set_required_interface_class(HCI_CLASS);
  }

  /// An HCI command: opcode (little endian), parameter length, parameters.
  bool send_command(const uint8_t *packet, size_t len);

 protected:
  static constexpr uint8_t HCI_CLASS = 0xE0;
  static constexpr size_t EVENT_MAX = 2 + 255;
  static constexpr size_t ACL_MAX = 4 + 1021;

  void on_connected() override;
  void on_disconnected() override;
  bool reports_connection_itself() const override { return true; }

  bool find_interface_();
  void read_event_(uint32_t generation);
  void read_acl_(uint32_t generation);
  // Main loop.
  void handle_event_(const std::vector<uint8_t> &event);
  void probe_next_();
  void probe_done_(bool ok);

  uint8_t intf_{0xFF};
  uint8_t evt_ep_{0};
  uint16_t evt_mps_{0};
  uint8_t acl_in_ep_{0};
  uint16_t acl_in_mps_{0};
  uint8_t acl_out_ep_{0};
  bool vendor_class_{false};

  // Bumped on connect and disconnect: completions of an earlier device are dropped.
  std::atomic<uint32_t> generation_{0};

  // Reassembly, USB task.
  uint8_t event_[EVENT_MAX]{};
  size_t event_len_{0};
  uint8_t acl_[ACL_MAX]{};
  size_t acl_len_{0};

  // Probe, main loop.
  uint8_t probe_step_{0};
  uint16_t probe_opcode_{0};
  uint16_t manufacturer_{0};
};

}  // namespace esphome::usb_bluetooth

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
