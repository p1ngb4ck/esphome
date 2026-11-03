#pragma once

#include "esphome/components/usb_hid/usb_hid.h"
#include "esphome/core/automation.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <string>
#include <vector>

namespace esphome::usb_hid {

// One button, identified by a bit in the interrupt IN report, for a device whose report
// descriptor says nothing useful about it. Turn on log_reports and read byte and mask off the log.
//
// Short and long are decided on release, not on press. While a button is held nothing is
// known yet, so acting on the press would fire the short action first and the long one after
// it. The device does not report a duration; the two edges of the bit are all there is.
struct HIDRawButton {
  void set_byte_index(uint8_t byte_index) { this->byte_index = byte_index; }
  void set_mask(uint8_t mask) { this->mask = mask; }
  void set_on_short_press(Trigger<> *trigger) { this->on_short_press = trigger; }
  void set_on_long_press(Trigger<> *trigger) { this->on_long_press = trigger; }

  uint8_t byte_index{0};
  uint8_t mask{0};
  bool pressed{false};
  uint32_t pressed_at_ms{0};
  Trigger<> *on_short_press{nullptr};
  Trigger<> *on_long_press{nullptr};
};

// Every report of the device, as it arrives, beside what its report descriptor makes of it.
class RawHIDDriver {
 public:
  explicit RawHIDDriver(USBHIDClient *parent) : parent_(parent) {}

  void set_log_reports(bool log_reports) { this->log_reports_ = log_reports; }
  void set_long_press_time(uint32_t ms) { this->long_press_time_ = ms; }
  void set_on_report(Trigger<std::vector<uint8_t>> *trigger) { this->on_report_ = trigger; }
  void add_button(HIDRawButton *button) { this->buttons_.push_back(button); }

  void process_report(const uint8_t *data, size_t len) {
    if (len == 0)
      return;

    if (this->log_reports_) {
      // Repeats are dropped rather than logged. A device that repeats while a button is held
      // would otherwise bury the transitions that are actually being looked for.
      std::vector<uint8_t> current(data, data + len);
      if (current != this->last_logged_) {
        std::string hex;
        char byte_text[4];
        for (size_t i = 0; i < len; i++) {
          snprintf(byte_text, sizeof(byte_text), "%02X", data[i]);
          if (i != 0)
            hex += ' ';
          hex += byte_text;
        }
        ESP_LOGI("usb_hid", "report (%u bytes): %s", static_cast<unsigned>(len), hex.c_str());
        this->last_logged_ = std::move(current);
      }
    }

    if (this->on_report_ != nullptr)
      this->on_report_->trigger(std::vector<uint8_t>(data, data + len));

    const uint32_t now = millis();
    for (auto *button : this->buttons_) {
      // A report shorter than the configured byte counts as not pressed, so a device that
      // sends a short all-clear report still produces a release edge.
      const bool down = button->byte_index < len && (data[button->byte_index] & button->mask) != 0;
      if (down == button->pressed)
        continue;
      button->pressed = down;
      if (down) {
        button->pressed_at_ms = now;
        continue;
      }
      const uint32_t held = now - button->pressed_at_ms;
      Trigger<> *trigger = held >= this->long_press_time_ ? button->on_long_press : button->on_short_press;
      ESP_LOGD("usb_hid", "button byte %u mask 0x%02X held %ums -> %s", button->byte_index, button->mask,
               static_cast<unsigned>(held), held >= this->long_press_time_ ? "long" : "short");
      if (trigger != nullptr)
        trigger->trigger();
    }
  }

 protected:
  USBHIDClient *parent_;
  bool log_reports_{false};
  uint32_t long_press_time_{600};
  Trigger<std::vector<uint8_t>> *on_report_{nullptr};
  std::vector<HIDRawButton *> buttons_;
  std::vector<uint8_t> last_logged_;
};

}  // namespace esphome::usb_hid
