#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

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
class USBHIDClient;

// One USB HID device of the client, allocated when it connects and freed once it is gone: every HID
// interface of it, each read through its own report descriptor (keyboards, mice, remotes, gamepads, and
// receivers that put several of them on one interface), and the gamepad driver of the pads it knows
// (hid/gamepad.h). Its own key state (modifiers, keys held, Num Lock), its keys, text, pointer and controls
// go to the client's input, which is what automations, sensors, LVGL and key_provider listen to.
class USBHIDDevice final : public hid::HIDInput {
  friend class USBHIDClient;
  friend class MCEReceiverDriver;

 public:
  USBHIDDevice(USBHIDClient *client, usb_host::UsbDevice *dev, uint32_t serial);

  uint8_t get_device_kinds() const override;
  // Gamepad driver transport: the pad's interface.
  bool send_output_report(const uint8_t *data, size_t len) override;
  bool set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) override;
  bool request_feature_report(uint8_t id, uint16_t len) override;
  Component *hid_component() override;

  const usb_device_desc_t *device_desc() const { return this->dev_->desc; }
  /// A control transfer on this device (its pad / remote drivers).
  bool control_transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                        const usb_host::transfer_cb_t &callback, const std::vector<uint8_t> &data = {},
                        int32_t w_length = -1);

 protected:
  static constexpr size_t MAX_INTERFACES = 4;

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

  void write_leds_(uint8_t leds) override;

  /// Main loop: the device was opened - its interfaces, then the report descriptors, then reading.
  bool start_();
  /// Main loop: the device is gone or let go - endpoints stopped, interfaces released.
  void stop_();
  bool find_interfaces_();
  void attach_pad_();
  void fetch_descriptor_(size_t index);
  void descriptor_done_(size_t index, const uint8_t *data, size_t length);
  void start_reading_();
  void read_(uint8_t index, uint8_t number, uint8_t ep, uint16_t mps, uint32_t generation);
  void report_(size_t index, const uint8_t *data, size_t length);
  void thrustmaster_();

  USBHIDClient *client_;
  usb_host::UsbDevice *dev_;
  uint32_t serial_;  // unique per connection: a report queued for a device that is gone finds none
  std::vector<Interface> interfaces_{};
  bool radio_{false};
  std::string device_key_;  // "usb:vvvv:pppp[:serial]"
  std::string device_name_;
  const char *device_label_() const override { return this->device_name_.c_str(); }
  std::atomic<uint32_t> generation_{0};
  size_t pad_index_{0};  // the interface the gamepad driver talks to
  bool ready_{false};
};

// USB HID: any number of HID devices at once (a keyboard, the receiver of a wireless set, a headset's
// buttons, pads), each a USBHIDDevice of its own, created when it connects - no entry per device. The
// component is the input everything listens to (hid_id): every device's keys, text, pointer and controls.
class USBHIDClient : public usb_host::USBClient, public hid::HIDInput {
  friend class USBHIDDevice;

 public:
  USBHIDClient(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  void register_raw_driver(RawHIDDriver *driver) { this->raw_driver_ = driver; }
  /// The connected device with this vid:pid (the first one), or nullptr: for lambdas (control transfers to it).
  USBHIDDevice *find_device(uint16_t vid, uint16_t pid) const;
  /// Main loop.
  uint8_t get_device_kinds() const override;
  /// The devices connected.
  void list_devices(std::vector<hid::InputDevice> &out) const override;
  uint8_t input_device_kinds() const;
  bool is_radio(const std::string &key) const;
  void set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) override;
  hid::HIDInput *device_input(const std::string &key) override;
#ifdef USE_TEXT_SENSOR
  void register_device_name_sensor(text_sensor::TextSensor *sensor) { this->device_name_sensor_ = sensor; }
#endif
  // The pads' outputs: to every device's pad.
  void rumble(uint8_t strong, uint8_t weak, uint32_t duration_ms) override;
  void set_player_led(uint8_t player) override;
  void set_light_bar(uint8_t red, uint8_t green, uint8_t blue) override;
  Component *hid_component() override { return this; }

 protected:
  static constexpr size_t REPORT_MAX = 64;
  static constexpr size_t QUEUE_SIZE = 32;

  // One interrupt IN report, from the USB task to the main loop.
  struct Report {
    uint32_t serial;  // the device's (USBHIDDevice::serial_)
    uint8_t interface;
    uint8_t length;
    uint8_t data[REPORT_MAX];
    void release() {}
  };

  bool accepts_more_devices_() const override { return true; }
  uint32_t skipped_endpoints_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                              const usb_intf_desc_t *alt) const override;
  void on_device_connected(usb_host::UsbDevice &dev) override;
  void on_device_disconnected(usb_host::UsbDevice &dev) override;
  void free_device_context(usb_host::UsbDevice &dev) override;
  void write_leds_(uint8_t leds) override;
  USBHIDDevice *find_device_(uint32_t serial) const;
  void publish_name_();

  std::vector<USBHIDDevice *> hid_devices_{};
  uint32_t next_serial_{1};
  LockFreeQueue<Report, QUEUE_SIZE> reports_{};
  EventPool<Report, QUEUE_SIZE - 1> report_pool_{};
  RawHIDDriver *raw_driver_{nullptr};
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *device_name_sensor_{nullptr};
#endif
};

}  // namespace esphome::usb_hid

#ifdef USB_HID_ENABLE_RAW
#include "raw_driver.h"
#endif

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31
