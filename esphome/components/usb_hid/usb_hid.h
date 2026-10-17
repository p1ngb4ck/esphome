#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/lock_free_queue.h"
#include "hid_types.h"
#include "mce.h"
#include "xbox.h"

#include <atomic>
#include <memory>
#include <vector>

#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome::usb_hid {

class RawHIDDriver;

// A USB HID device: every HID interface of it, each read through its own report descriptor
// (keyboards, mice, remotes, gamepads, and receivers that put several of them on one
// interface), into the shared hid::HIDInput, and through the gamepad driver of the pads it
// knows (hid/gamepad.h). Xbox 360 and Xbox One pads, which are not HID, have drivers here.
class USBHIDClient : public usb_host::USBClient, public hid::HIDInput {
  friend class MCEReceiverDriver;

 public:
  USBHIDClient(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  void register_raw_driver(RawHIDDriver *driver) { this->raw_driver_ = driver; }
  /// Main loop.
  uint8_t get_device_kinds() const override;
  /// The device connected, if one is.
  void list_devices(std::vector<hid::InputDevice> &out) const override;
  void set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) override;
  hid::HIDInput *device_input(const std::string &key) override {
    return !this->device_key_.empty() && key == this->device_key_ ? this : nullptr;
  }
#ifdef USE_TEXT_SENSOR
  void register_device_name_sensor(text_sensor::TextSensor *sensor) { this->device_name_sensor_ = sensor; }
#endif

  // Gamepad driver transport: the pad's interface.
  bool send_output_report(const uint8_t *data, size_t len) override;
  bool set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) override;
  bool request_feature_report(uint8_t id, uint16_t len) override;
  Component *hid_component() override { return this; }

 protected:
  static constexpr size_t MAX_INTERFACES = 4;
  static constexpr size_t REPORT_MAX = 64;
  static constexpr size_t QUEUE_SIZE = 32;

  // Interfaces that are not HID: pads with a protocol of their own.
  enum Vendor : uint8_t { VENDOR_NONE, VENDOR_XBOX360, VENDOR_XBOX360_WIRELESS, VENDOR_XBOXONE, VENDOR_MCE };

  struct Interface {
    uint8_t number{0};
    uint8_t protocol{0};
    uint8_t in_ep{0};
    uint16_t in_mps{0};
    uint8_t out_ep{0};
    uint16_t descriptor_length{0};
    Vendor vendor{VENDOR_NONE};
    std::unique_ptr<hid::HIDReportMap> map{};
  };

  // One interrupt IN report, from the USB task to the main loop.
  struct Report {
    uint8_t interface;
    uint8_t length;
    uint8_t data[REPORT_MAX];
    void release() {}
  };

  void on_connected() override;
  void on_disconnected() override;
  bool reports_connection_itself() const override { return true; }
  void write_leds_(uint8_t leds) override;

  bool find_interfaces_();
  void fetch_descriptor_(size_t index);
  void descriptor_done_(size_t index, const uint8_t *data, size_t length);
  void start_reading_();
  void read_(uint8_t index, uint8_t number, uint8_t ep, uint16_t mps, uint32_t generation);
  void report_(size_t index, const uint8_t *data, size_t length);
  void attach_pad_();
  void thrustmaster_();

  std::vector<Interface> interfaces_{};
  std::string device_key_;  // the device connected: "usb:vvvv:pppp[:serial]", "" none
  std::string device_name_;
  std::atomic<uint32_t> generation_{0};
  LockFreeQueue<Report, QUEUE_SIZE> reports_{};
  EventPool<Report, QUEUE_SIZE - 1> report_pool_{};
  RawHIDDriver *raw_driver_{nullptr};
  size_t pad_index_{0};  // the interface the gamepad driver talks to
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *device_name_sensor_{nullptr};
#endif
};

}  // namespace esphome::usb_hid

#ifdef USB_HID_ENABLE_RAW
#include "raw_driver.h"
#endif

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
