#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/components/usb_host/usb_host.h"
#include "hid_types.h"
#include <map>
#include <vector>

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

namespace esphome {
namespace usb_hid {

class HIDDeviceDriver;
class USBHIDClient;

// Tracks one connected HID device (one interface → one interrupt IN transfer slot)
struct HIDDevice {
  usb_device_handle_t dev_hdl{nullptr};
  uint8_t interface_num{0};
  uint8_t in_ep{0};
  uint8_t out_ep{0};
  uint8_t media_in_ep{0};
  uint8_t dev_addr{0};
  uint16_t vid{0};
  uint16_t pid{0};
  uint8_t protocol{0};
  bool active{false};
  bool media_active{false};
  HIDDeviceDriver *driver{nullptr};
};

// Pluggable driver interface — implement to handle a specific HID device class
class HIDDeviceDriver {
 public:
  virtual ~HIDDeviceDriver() = default;
  virtual bool match_device(uint8_t protocol, uint16_t vid, uint16_t pid) = 0;
  virtual void process_report(const uint8_t *data, size_t len, HIDDevice *device) = 0;
  virtual const char *get_name() = 0;
};

class USBHIDClient : public usb_host::USBClient {
 public:
  USBHIDClient() : usb_host::USBClient(0, 0) {}

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  // Return ANY so that vendor-specific devices (Xbox360) are not rejected by the
  // class filter in USBClient. The actual class check is done in parse_hid_interface_().
  uint8_t get_interface_class() const override { return usb_host::USB_INTERFACE_CLASS_ANY; }

  void register_device_driver(HIDDeviceDriver *driver) { this->drivers_.push_back(driver); }

#ifdef USE_TEXT_SENSOR
  void register_keyboard_sensor(text_sensor::TextSensor *sensor) { this->keyboard_sensor_ = sensor; }
  text_sensor::TextSensor *get_keyboard_sensor() { return this->keyboard_sensor_; }
  void register_device_name_sensor(text_sensor::TextSensor *sensor) { this->device_name_sensor_ = sensor; }
  text_sensor::TextSensor *get_device_name_sensor() { return this->device_name_sensor_; }
#endif

#ifdef USE_BINARY_SENSOR
  void register_keyboard_key_sensor(binary_sensor::BinarySensor *sensor, uint8_t keycode);
  std::map<uint8_t, binary_sensor::BinarySensor *> &get_keyboard_key_sensors() {
    return this->keyboard_key_sensors_;
  }
  void register_mouse_left_sensor(binary_sensor::BinarySensor *sensor) { this->mouse_left_sensor_ = sensor; }
  void register_mouse_right_sensor(binary_sensor::BinarySensor *sensor) { this->mouse_right_sensor_ = sensor; }
  void register_mouse_middle_sensor(binary_sensor::BinarySensor *sensor) { this->mouse_middle_sensor_ = sensor; }
  binary_sensor::BinarySensor *get_mouse_left_sensor() { return this->mouse_left_sensor_; }
  binary_sensor::BinarySensor *get_mouse_right_sensor() { return this->mouse_right_sensor_; }
  binary_sensor::BinarySensor *get_mouse_middle_sensor() { return this->mouse_middle_sensor_; }
#endif

#ifdef USE_SENSOR
  void register_mouse_x_sensor(sensor::Sensor *sensor) { this->mouse_x_sensor_ = sensor; }
  void register_mouse_y_sensor(sensor::Sensor *sensor) { this->mouse_y_sensor_ = sensor; }
  void register_mouse_wheel_sensor(sensor::Sensor *sensor) { this->mouse_wheel_sensor_ = sensor; }
  sensor::Sensor *get_mouse_x_sensor() { return this->mouse_x_sensor_; }
  sensor::Sensor *get_mouse_y_sensor() { return this->mouse_y_sensor_; }
  sensor::Sensor *get_mouse_wheel_sensor() { return this->mouse_wheel_sensor_; }
#endif

  // LED / output report helpers (called from drivers, safe from main-loop context)
  void update_keyboard_leds(HIDDevice *device, uint8_t led_state);
  void send_output_report(HIDDevice *device, const uint8_t *data, size_t len);

 protected:
  void on_connected() override;
  void on_disconnected() override;

  bool parse_hid_interface_();
  bool setup_media_interface_(HIDDevice *dev);
  void on_transfer_in_(uint8_t ep, const uint8_t *data, size_t len, bool media);
  void start_in_transfer_(HIDDevice *dev, uint8_t ep, uint16_t mps, bool media);

  HIDDevice devices_[HID_MAX_DEVICES]{};
  int connected_devices_{0};

  std::vector<HIDDeviceDriver *> drivers_;

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *keyboard_sensor_{nullptr};
  text_sensor::TextSensor *device_name_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  std::map<uint8_t, binary_sensor::BinarySensor *> keyboard_key_sensors_;
  binary_sensor::BinarySensor *mouse_left_sensor_{nullptr};
  binary_sensor::BinarySensor *mouse_right_sensor_{nullptr};
  binary_sensor::BinarySensor *mouse_middle_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *mouse_x_sensor_{nullptr};
  sensor::Sensor *mouse_y_sensor_{nullptr};
  sensor::Sensor *mouse_wheel_sensor_{nullptr};
#endif
};

}  // namespace usb_hid
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_*
