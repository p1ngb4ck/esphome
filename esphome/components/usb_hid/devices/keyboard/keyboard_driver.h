#pragma once

#include "esphome/components/usb_hid/usb_hid.h"

namespace esphome {
namespace usb_hid {

class KeyboardDriver : public HIDDeviceDriver {
 public:
  explicit KeyboardDriver(USBHIDClient *parent) : parent_(parent) {}

  bool match_device(uint8_t protocol, uint16_t vid, uint16_t pid) override {
    // Xbox 360 wired/wireless — skip, not a keyboard
    if (vid == XBOX360_VID && (pid == XBOX360_PID_WIRED || pid == XBOX360_PID_WIRELESS))
      return false;
    if (vid == BITDO_VID && pid == BITDO_PID_XBOX)
      return false;
    return protocol == HID_PROTO_KEYBOARD;
  }

  void process_report(const uint8_t *data, size_t len, HIDDevice *device) override {
    // Media report marker set by USBHIDClient::on_transfer_in_()
    if (len >= 2 && data[0] == HID_MEDIA_REPORT_MARKER) {
      process_media_report_(&data[1], len - 1);
      return;
    }

    // Standard boot keyboard report: 8 bytes
    if (len < 8)
      return;

    // Skip error roll-over codes
    for (int i = 2; i < 8; i++) {
      if (data[i] >= 0x01 && data[i] <= 0x03)
        return;
    }

    uint8_t modifier = data[0];
    if (modifier != this->prev_modifier_) {
      uint8_t changed = modifier ^ this->prev_modifier_;
      auto pub = [&](const char *name, bool pressed) {
        ESP_LOGI("KeyboardDriver", "%s %s", name, pressed ? "pressed" : "released");
#ifdef USE_TEXT_SENSOR
        if (pressed && this->parent_->get_keyboard_sensor())
          this->parent_->get_keyboard_sensor()->publish_state(name);
#endif
      };
      if (changed & 0x01) pub("Left Ctrl", modifier & 0x01);
      if (changed & 0x02) pub("Left Shift", modifier & 0x02);
      if (changed & 0x04) pub("Left Alt", modifier & 0x04);
      if (changed & 0x08) pub("Win", modifier & 0x08);
      if (changed & 0x10) pub("Right Ctrl", modifier & 0x10);
      if (changed & 0x20) pub("Right Shift", modifier & 0x20);
      if (changed & 0x40) pub("Right Alt", modifier & 0x40);
      if (changed & 0x80) pub("Right Win", modifier & 0x80);
      this->prev_modifier_ = modifier;
    }

#ifdef USE_BINARY_SENSOR
    for (auto &kv : this->parent_->get_keyboard_key_sensors()) {
      uint8_t target = kv.first;
      bool pressed = false;
      for (int i = 2; i < 8; i++) {
        if (data[i] == target) {
          pressed = true;
          break;
        }
      }
      kv.second->publish_state(pressed);
    }
#endif

    bool shift = (modifier & 0x22) != 0;
    bool win_key = (modifier & 0x08) != 0;
    bool ctrl_key = (modifier & 0x11) != 0;
    uint32_t now = esphome::millis();

    for (int i = 2; i < 8; i++) {
      if (data[i] == 0)
        continue;

      bool was_pressed = false;
      for (int j = 0; j < 6; j++) {
        if (this->prev_keys_[j] == data[i]) {
          was_pressed = true;
          break;
        }
      }
      if (was_pressed) {
        // Simple repeat suppression
        if (now - this->last_key_time_ < 500)
          continue;
      } else {
        this->last_key_time_ = now;
      }

      // Windows key combinations
      if (win_key && !ctrl_key) {
        const char *combo = nullptr;
        switch (data[i]) {
          case 0x07: combo = "Show Desktop (Win+D)"; break;
          case 0x0B: combo = "Dictation (Win+H)"; break;
          case 0x0E: combo = "Connect (Win+K)"; break;
          case 0x0C: combo = "Settings (Win+I)"; break;
        }
        if (combo) {
          publish_key_(combo);
          memcpy(this->prev_keys_, &data[2], 6);
          return;
        }
      }

      // Lock keys
      if (data[i] == 0x39) {
        this->caps_lock_ = !this->caps_lock_;
        update_leds_(device);
        publish_key_(this->caps_lock_ ? "Caps Lock ON" : "Caps Lock OFF");
      } else if (data[i] == 0x53) {
        this->num_lock_ = !this->num_lock_;
        update_leds_(device);
        publish_key_(this->num_lock_ ? "Num Lock ON" : "Num Lock OFF");
      } else if (data[i] == 0x47) {
        this->scroll_lock_ = !this->scroll_lock_;
        update_leds_(device);
        publish_key_(this->scroll_lock_ ? "Scroll Lock ON" : "Scroll Lock OFF");
      } else {
        char ascii = hid_to_ascii_(data[i], shift);
        if (ctrl_key && ascii >= 'a' && ascii <= 'z')
          ascii = ascii - 'a' + 1;
        else if (ctrl_key && ascii >= 'A' && ascii <= 'Z')
          ascii = ascii - 'A' + 1;

        if (ascii != 0) {
          const char *name = nullptr;
          if      (ascii == '\b') name = "Backspace";
          else if (ascii == '\t') name = "Tab";
          else if (ascii == '\n') name = "Enter";
          else if (ascii == 0x1B) name = "Escape";
          if (name) {
            publish_key_(name);
          } else {
            char buf[2] = {ascii, 0};
            publish_key_(buf);
          }
        } else {
          const char *key_name = named_key_(data[i]);
          if (key_name)
            publish_key_(key_name);
        }
      }
    }
    memcpy(this->prev_keys_, &data[2], 6);
  }

  const char *get_name() override { return "Keyboard"; }

 protected:
  USBHIDClient *parent_;
  uint8_t prev_keys_[6]{0};
  uint8_t prev_modifier_{0};
  bool caps_lock_{false};
  bool num_lock_{false};
  bool scroll_lock_{false};
  uint32_t last_key_time_{0};

  void publish_key_(const char *name) {
    ESP_LOGI("KeyboardDriver", "Key: %s", name);
#ifdef USE_TEXT_SENSOR
    if (this->parent_->get_keyboard_sensor())
      this->parent_->get_keyboard_sensor()->publish_state(name);
#endif
  }

  void update_leds_(HIDDevice *device) {
    uint8_t led = 0;
    if (this->num_lock_)    led |= HID_KB_LED_NUM_LOCK;
    if (this->caps_lock_)   led |= HID_KB_LED_CAPS_LOCK;
    if (this->scroll_lock_) led |= HID_KB_LED_SCROLL_LOCK;
    this->parent_->update_keyboard_leds(device, led);
  }

  static const char *named_key_(uint8_t k) {
    switch (k) {
      case 0x29: return "Escape";
      case 0x3A: return "F1";   case 0x3B: return "F2";  case 0x3C: return "F3";
      case 0x3D: return "F4";   case 0x3E: return "F5";  case 0x3F: return "F6";
      case 0x40: return "F7";   case 0x41: return "F8";  case 0x42: return "F9";
      case 0x43: return "F10";  case 0x44: return "F11"; case 0x45: return "F12";
      case 0x46: return "Print Screen";
      case 0x47: return "Scroll Lock";
      case 0x48: return "Pause";
      case 0x49: return "Insert";
      case 0x4A: return "Home";
      case 0x4B: return "Page Up";
      case 0x4C: return "Delete";
      case 0x4D: return "End";
      case 0x4E: return "Page Down";
      case 0x4F: return "Right";
      case 0x50: return "Left";
      case 0x51: return "Down";
      case 0x52: return "Up";
      case 0x65: return "Menu";
      default:   return nullptr;
    }
  }

  char hid_to_ascii_(uint8_t k, bool shift) {
    if (k >= 0x04 && k <= 0x1D) {
      char c = 'a' + (k - 0x04);
      return (shift ^ this->caps_lock_) ? static_cast<char>(c - 32) : c;
    }
    if (k >= 0x1E && k <= 0x27) {
      const char nums[]    = "1234567890";
      const char shifted[] = "!@#$%^&*()";
      return shift ? shifted[k - 0x1E] : nums[k - 0x1E];
    }
    switch (k) {
      case 0x2C: return ' ';
      case 0x28: return '\n';
      case 0x2A: return '\b';
      case 0x2B: return '\t';
      case 0x2D: return shift ? '_' : '-';
      case 0x2E: return shift ? '+' : '=';
      case 0x2F: return shift ? '{' : '[';
      case 0x30: return shift ? '}' : ']';
      case 0x31: return shift ? '|' : '\\';
      case 0x33: return shift ? ':' : ';';
      case 0x34: return shift ? '"' : '\'';
      case 0x35: return shift ? '~' : '`';
      case 0x36: return shift ? '<' : ',';
      case 0x37: return shift ? '>' : '.';
      case 0x38: return shift ? '?' : '/';
      case 0x54: return '/';
      case 0x55: return '*';
      case 0x56: return '-';
      case 0x57: return '+';
      case 0x58: return '\n';
      case 0x59: return this->num_lock_ ? '1' : 0;
      case 0x5A: return this->num_lock_ ? '2' : 0;
      case 0x5B: return this->num_lock_ ? '3' : 0;
      case 0x5C: return this->num_lock_ ? '4' : 0;
      case 0x5D: return this->num_lock_ ? '5' : 0;
      case 0x5E: return this->num_lock_ ? '6' : 0;
      case 0x5F: return this->num_lock_ ? '7' : 0;
      case 0x60: return this->num_lock_ ? '8' : 0;
      case 0x61: return this->num_lock_ ? '9' : 0;
      case 0x62: return this->num_lock_ ? '0' : 0;
      case 0x63: return this->num_lock_ ? '.' : 0;
      default:   return 0;
    }
  }

  void process_media_report_(const uint8_t *data, size_t len) {
    if (len < 1)
      return;
    // Consumer control: first byte is a consumer usage code
    const char *key_name = nullptr;
    switch (data[0]) {
      case 0xE9: key_name = "Volume Up";      break;
      case 0xEA: key_name = "Volume Down";    break;
      case 0xE2: key_name = "Mute";           break;
      case 0xCD: key_name = "Play/Pause";     break;
      case 0xB5: key_name = "Next Track";     break;
      case 0xB6: key_name = "Prev Track";     break;
      case 0xB7: key_name = "Stop";           break;
      case 0x8A: key_name = "Mail";           break;
      case 0x92: key_name = "Calculator";     break;
      case 0x94: key_name = "My Computer";    break;
      case 0x23: key_name = "WWW Home";       break;
      case 0x21: key_name = "WWW Search";     break;
      case 0x24: key_name = "WWW Back";       break;
      case 0x25: key_name = "WWW Forward";    break;
    }
    if (key_name)
      publish_key_(key_name);
  }
};

}  // namespace usb_hid
}  // namespace esphome
