#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace esphome::usb_bluetooth {

// A USB Bluetooth dongle as a usb_host client (Bluetooth Core Specification, USB transport):
// HCI commands as class requests on endpoint 0, events on the interrupt IN endpoint, ACL data
// on the bulk pair of the controller interface (E0/01/01, or FF/01/01 for a vendor-class
// controller from the match table or named by vid/pid).
//
// Reads are one wMaxPacketSize packet each and reassembled by the packet's own length field:
// an event or ACL packet whose length is a whole number of USB packets ends without a short
// packet, so a larger read would wait for data that is not coming.
//
// On connect the controller is reset and asked for its version, address and name (logged).
// With bluedroid: true it then becomes ESP-IDF Bluedroid's controller (esp_bluedroid_hci.h, the
// host-only transport): H4 packets from Bluedroid are split onto endpoint 0 (commands) and the
// bulk OUT endpoint (ACL); events and ACL data go back up with their H4 type byte.
class USBBluetooth : public usb_host::USBClient {
 public:
  USBBluetooth(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {
    this->add_class_match(HCI_CLASS, 0x01, 0x01);
    // Controllers behind the vendor class, as Linux btusb.c matches them.
    this->add_vendor_match(0x0A5C, 0, 0xFF, 0x01, 0x01);  // Broadcom
    this->add_vendor_match(0x05AC, 0, 0xFF, 0x01, 0x01);  // Apple
  }

  /// An HCI command: opcode (little endian), parameter length, parameters.
  bool send_command(const uint8_t *packet, size_t len);

  /// The controller's manufacturer (Bluetooth SIG company identifier), once it is probed.
  uint16_t manufacturer() const { return this->manufacturer_; }

  /// A Realtek patch for one ROM version of the chip with this lmp_subver (prepared by realtek.py).
  void add_realtek_firmware(uint16_t lmp_subver, uint8_t rom_version, const uint8_t *data, size_t length) {
    this->realtek_images_.push_back({lmp_subver, rom_version, data, length});
  }

#ifdef USE_USB_BLUETOOTH_H4
  // Bluedroid's transport operations; its HCI task calls these.
  void hci_send(uint8_t *data, uint16_t len);
  bool hci_send_available();
#endif

 protected:
  static constexpr uint8_t HCI_CLASS = 0xE0;
  static constexpr size_t EVENT_MAX = 2 + 255;
  static constexpr size_t ACL_MAX = 4 + 1021;

  void on_connected() override;
  void on_disconnected() override;
  bool reports_connection_itself() const override { return true; }
  bool starting_() const override;
  uint32_t skipped_endpoints_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                              const usb_intf_desc_t *alt) const override;

  bool find_interface_();
  void read_event_(uint32_t generation);
  void read_acl_(uint32_t generation);
  // Main loop.
  void handle_event_(const std::vector<uint8_t> &event);
  // One command; then runs with its return parameters once it completed with status 0.
  void ask_(uint16_t opcode, std::vector<uint8_t> params, std::function<void(const uint8_t *ret, size_t n)> &&then);
  void probe_start_();
  void probe_identity_();
  void realtek_();
  void download_fragment_(uint32_t i);
  void probe_done_(bool ok);
  // USB task: a command or ACL transfer finished.
  void send_done_();
#ifdef USE_USB_BLUETOOTH_BLUEDROID
  void start_bluedroid_();
#endif
#ifdef USE_USB_BLUETOOTH_H4
  bool send_acl_(const uint8_t *data, size_t len);
  // USB task: an H4 packet from the dongle to the host stack.
  void to_stack_(uint8_t *h4, size_t len);
#endif
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
  void start_split_();
#endif

  uint8_t intf_{0xFF};
  uint8_t evt_ep_{0};
  uint16_t evt_mps_{0};
  uint8_t acl_in_ep_{0};
  uint16_t acl_in_mps_{0};
  uint8_t acl_out_ep_{0};
  uint16_t acl_out_mps_{0};
  bool vendor_class_{false};

  // Bumped on connect and disconnect: completions of an earlier device are dropped.
  std::atomic<uint32_t> generation_{0};

  // Reassembly, USB task. Byte 0 is kept free for the H4 packet type.
  uint8_t event_[1 + EVENT_MAX]{};
  size_t event_len_{0};
  uint8_t acl_[1 + ACL_MAX]{};
  size_t acl_len_{0};

  // The controller interface is claimed and the readers run.
  std::atomic<bool> link_up_{false};
  // Events and ACL data go to Bluedroid rather than to the probe.
  std::atomic<bool> stack_attached_{false};
  // Bluedroid refused a send for want of transfer slots; tell it once one frees up.
  std::atomic<bool> send_blocked_{false};
#ifdef USE_BT_PERSISTENCE
  // Bluedroid's start waits for its pairing data.
  std::atomic<bool> waiting_for_store_{false};
#endif

  struct RealtekImage {
    uint16_t lmp_subver;
    uint8_t rom_version;
    const uint8_t *data;
    size_t length;
  };
  std::vector<RealtekImage> realtek_images_{};

  // Probe, main loop.
  uint16_t pending_opcode_{0};
  std::function<void(const uint8_t *, size_t)> pending_{};
  uint16_t manufacturer_{0};
  uint16_t lmp_subver_{0};
  const RealtekImage *download_{nullptr};
};

}  // namespace esphome::usb_bluetooth

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
