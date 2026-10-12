#include "hid_input.h"

#include <algorithm>
#include <cstdio>

namespace esphome::hid {

static constexpr uint16_t USAGE_POINTER = 0x01;
static constexpr uint16_t USAGE_MOUSE = 0x02;
static constexpr uint16_t USAGE_X = 0x30;
static constexpr uint16_t USAGE_Y = 0x31;
static constexpr uint16_t USAGE_WHEEL = 0x38;
static constexpr uint16_t USAGE_AC_PAN = 0x238;

static constexpr uint16_t KEY_ENTER = 0x28;
static constexpr uint16_t KEY_BACKSPACE = 0x2A;
static constexpr uint16_t KEY_TAB = 0x2B;
static constexpr uint16_t KEY_SPACE = 0x2C;
static constexpr uint16_t KEY_CAPS_LOCK = 0x39;
static constexpr uint16_t KEY_SCROLL_LOCK = 0x47;
static constexpr uint16_t KEY_NUM_LOCK = 0x53;
static constexpr uint16_t KEY_KP_ENTER = 0x58;
static constexpr uint16_t KEY_LEFT_CTRL = 0xE0;
static constexpr uint16_t KEY_RIGHT_GUI = 0xE7;

std::string key_name(uint16_t page, uint16_t usage) {
  for (uint16_t i = 0; i < KEY_NAMES_COUNT; i++) {
    if (KEY_NAMES[i].page == page && KEY_NAMES[i].usage == usage)
      return KEY_NAMES[i].name;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%02X:0x%02X", page, usage);
  return buf;
}

void append_utf8(std::string &out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

bool HIDInput::is_key_down(uint16_t page, uint16_t usage) const {
  const uint32_t key = static_cast<uint32_t>(page) << 16 | usage;
  return std::find(this->down_.begin(), this->down_.end(), key) != this->down_.end();
}

void HIDInput::process_hid_values(const std::vector<HIDReportItemValue> &values) {
  int32_t dx = 0, dy = 0, wheel = 0, pan = 0;
  bool pointer = false;
  for (const auto &v : values) {
    const uint16_t page = v.usage.page;
    const uint16_t usage = v.usage.usage;
    const bool in_pointer = v.application_usage.page == PAGE_GENERIC_DESKTOP &&
                            (v.application_usage.usage == USAGE_MOUSE || v.application_usage.usage == USAGE_POINTER);
    if (page == PAGE_KEYBOARD || page == PAGE_CONSUMER) {
      if (page == PAGE_CONSUMER && v.is_relative) {
        if (usage == USAGE_AC_PAN) {
          pan += static_cast<int32_t>(v.value);
          pointer = true;
        } else {
          this->control_cb_.call(page, usage, static_cast<int32_t>(v.value));
        }
        continue;
      }
      this->key_(page, usage, v.value != 0);
      continue;
    }
    if (page == PAGE_BUTTON) {
      if (in_pointer && usage >= 1 && usage <= 8) {
        const uint8_t bit = 1u << (usage - 1);
        this->buttons_ = v.value != 0 ? (this->buttons_ | bit) : (this->buttons_ & ~bit);
        pointer = true;
      }
      this->key_(page, usage, v.value != 0);
      continue;
    }
    if (page == PAGE_GENERIC_DESKTOP && in_pointer && v.is_relative &&
        (usage == USAGE_X || usage == USAGE_Y || usage == USAGE_WHEEL)) {
      const auto value = static_cast<int32_t>(v.value);
      (usage == USAGE_X ? dx : usage == USAGE_Y ? dy : wheel) += value;
      pointer = true;
      continue;
    }
    // System Power Down, Sleep, Wake Up and the rest of the system controls are buttons.
    if (page == PAGE_GENERIC_DESKTOP && usage >= 0x81 && usage <= 0xB7) {
      this->key_(page, usage, v.value != 0);
      continue;
    }
    this->control_cb_.call(page, usage, static_cast<int32_t>(v.value));
  }
  if (pointer) {
    auto clamp16 = [](int32_t x) { return static_cast<int16_t>(std::clamp<int32_t>(x, INT16_MIN, INT16_MAX)); };
    this->pointer_cb_.call(clamp16(dx), clamp16(dy), clamp16(wheel), clamp16(pan), this->buttons_);
  }
}

void HIDInput::release_all_hid() {
  const auto down = this->down_;
  for (auto it = down.rbegin(); it != down.rend(); ++it)
    this->key_(*it >> 16, *it & 0xFFFF, false);
  if (this->buttons_ != 0) {
    this->buttons_ = 0;
    this->pointer_cb_.call(0, 0, 0, 0, 0);
  }
  this->modifiers_ = 0;
  this->dead_ = 0;
}

void HIDInput::key_(uint16_t page, uint16_t usage, bool pressed) {
  const uint32_t key = static_cast<uint32_t>(page) << 16 | usage;
  auto it = std::find(this->down_.begin(), this->down_.end(), key);
  if (pressed == (it != this->down_.end()))
    return;
  if (pressed) {
    this->down_.push_back(key);
  } else {
    this->down_.erase(it);
  }
  if (page == PAGE_KEYBOARD && usage >= KEY_LEFT_CTRL && usage <= KEY_RIGHT_GUI) {
    const uint8_t bit = 1u << (usage - KEY_LEFT_CTRL);
    this->modifiers_ = pressed ? (this->modifiers_ | bit) : (this->modifiers_ & ~bit);
  }
  this->key_cb_.call(page, usage, pressed);
  if (pressed && page == PAGE_KEYBOARD)
    this->type_key_(usage);
}

void HIDInput::type_key_(uint16_t usage) {
  uint8_t led = 0;
  if (usage == KEY_CAPS_LOCK) {
    led = LED_CAPS_LOCK;
  } else if (usage == KEY_NUM_LOCK) {
    led = LED_NUM_LOCK;
  } else if (usage == KEY_SCROLL_LOCK) {
    led = LED_SCROLL_LOCK;
  }
  if (led != 0) {
    this->leds_ ^= led;
    this->write_leds_(this->leds_);
    return;
  }
  if (usage == KEY_BACKSPACE) {
    this->dead_ = 0;
    this->send_key_('\b');
    return;
  }
  // Shortcuts type nothing; AltGr (Right Alt) is a level, not a shortcut.
  if ((this->modifiers_ & (MOD_LEFT_CTRL | MOD_RIGHT_CTRL | MOD_LEFT_ALT | MOD_LEFT_GUI | MOD_RIGHT_GUI)) != 0)
    return;
  if (usage == KEY_ENTER || usage == KEY_KP_ENTER) {
    this->type_('\n');
    return;
  }
  if (usage == KEY_TAB) {
    this->type_('\t');
    return;
  }
  if (usage == KEY_SPACE) {
    this->type_(' ');
    return;
  }
  // Keypad: the same on every layout; digits only with Num Lock on.
  if (usage >= 0x54 && usage <= 0x63) {
    static constexpr char KEYPAD[] = "/*-+\n1234567890.";
    if (usage <= 0x57 || (this->leds_ & LED_NUM_LOCK) != 0)
      this->type_(KEYPAD[usage - 0x54]);
    return;
  }
  if (this->layout_ == nullptr)
    return;
  const LayoutKey *key = nullptr;
  for (uint16_t i = 0; i < this->layout_->key_count; i++) {
    if (this->layout_->keys[i].usage == usage) {
      key = &this->layout_->keys[i];
      break;
    }
  }
  if (key == nullptr)
    return;
  bool shift = (this->modifiers_ & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT)) != 0;
  if (key->alphabetic && (this->leds_ & LED_CAPS_LOCK) != 0)
    shift = !shift;
  const uint8_t level = ((this->modifiers_ & MOD_RIGHT_ALT) != 0 ? 2 : 0) + (shift ? 1 : 0);
  const char32_t c = key->chars[level];
  if (c == 0)
    return;
  if ((key->dead & (1u << level)) != 0) {
    if (this->dead_ == 0) {
      this->dead_ = c;
      return;
    }
    // A dead key twice types its spacing form; another one types the first and waits again.
    const char32_t pending = this->dead_;
    this->dead_ = 0;
    const char32_t twice = this->compose_(pending, c);
    if (twice != 0) {
      this->type_(twice);
      return;
    }
    const char32_t alone = this->compose_(pending, ' ');
    if (alone != 0)
      this->type_(alone);
    this->dead_ = c;
    return;
  }
  this->type_(c);
}

char32_t HIDInput::compose_(char32_t mark, char32_t base) const {
  if (this->layout_ == nullptr)
    return 0;
  for (uint16_t i = 0; i < this->layout_->compose_count; i++) {
    const auto &entry = this->layout_->compose[i];
    if (entry.mark == mark && entry.base == base)
      return entry.result;
  }
  return 0;
}

void HIDInput::type_(char32_t c) {
  std::string text;
  if (this->dead_ != 0) {
    const char32_t mark = this->dead_;
    this->dead_ = 0;
    const char32_t composed = this->compose_(mark, c);
    if (composed != 0) {
      c = composed;
    } else {
      // Nothing to put the mark on: it is typed on its own, followed by the key.
      const char32_t alone = this->compose_(mark, ' ');
      if (alone != 0) {
        append_utf8(text, alone);
        if (alone < 0x80)
          this->send_key_(static_cast<uint8_t>(alone));
      }
    }
  }
  append_utf8(text, c);
  if (c < 0x80)
    this->send_key_(static_cast<uint8_t>(c));
  this->text_cb_.call(text);
}

}  // namespace esphome::hid
