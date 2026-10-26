#include "hid_input.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esphome::hid {

static const char *const TAG = "hid";

static std::atomic<DirectKeySink *> direct_sink{nullptr};

void set_direct_key_sink(DirectKeySink *sink) { direct_sink.store(sink, std::memory_order_release); }

static constexpr uint16_t USAGE_POINTER = 0x01;
static constexpr uint16_t USAGE_MOUSE = 0x02;
static constexpr uint16_t USAGE_X = 0x30;
static constexpr uint16_t USAGE_Y = 0x31;
static constexpr uint16_t USAGE_WHEEL = 0x38;
static constexpr uint16_t USAGE_AC_PAN = 0x238;
static constexpr uint16_t USAGE_HAT_SWITCH = 0x39;

// Joystick, gamepad, multi-axis controller.
static bool pad_application(const HIDUsage &app) {
  return app.page == PAGE_GENERIC_DESKTOP && (app.usage == 0x04 || app.usage == 0x05 || app.usage == 0x08);
}

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

bool build_led_report(const HIDReportMap &map, uint8_t leds, uint8_t &report_id, std::vector<uint8_t> &payload) {
  for (const auto &report : map.reports()) {
    if (report.kind != HIDReportKind::OUTPUT)
      continue;
    payload.assign(report.byte_size(), 0);
    bool has_led = false;
    for (const uint16_t id : report.field_ids) {
      const HIDFieldSchema *field = map.find_field(id);
      if (field == nullptr || field->is_constant() || !field->is_variable() || field->report_size != 1)
        continue;
      for (uint32_t i = 0; i < field->report_count; i++) {
        const HIDUsage usage = field->usage_at(i, true);
        if (usage.page != PAGE_LED || usage.usage < 1 || usage.usage > 3)
          continue;
        has_led = true;
        if ((leds & (1u << (usage.usage - 1))) == 0)
          continue;
        const uint32_t bit = field->bit_offset + i;
        if (bit / 8 < payload.size())
          payload[bit / 8] |= 1u << (bit % 8);
      }
    }
    if (has_led) {
      report_id = report.report_id;
      return true;
    }
  }
  payload.clear();
  return false;
}

bool parse_layout(const uint8_t *data, size_t len, KeyboardLayout *out) {
  static constexpr size_t HEADER = 8;
  if (data == nullptr || len < HEADER || (reinterpret_cast<uintptr_t>(data) & 3) != 0 ||
      std::memcmp(data, "KBL1", 4) != 0)
    return false;
  const uint16_t keys = data[4] | data[5] << 8;
  const uint16_t compose = data[6] | data[7] << 8;
  if (len != HEADER + keys * sizeof(LayoutKey) + compose * sizeof(LayoutCompose))
    return false;
  out->keys = reinterpret_cast<const LayoutKey *>(data + HEADER);
  out->key_count = keys;
  out->compose = reinterpret_cast<const LayoutCompose *>(data + HEADER + keys * sizeof(LayoutKey));
  out->compose_count = compose;
  return true;
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
  const uint8_t count = this->down_count_;
  for (uint8_t i = 0; i < count && i < MAX_DOWN; i++) {
    if (this->down_[i] == key)
      return true;
  }
  return false;
}

bool HIDInput::set_deferred(bool deferred) {
#ifdef USE_ESP32
  if (deferred == (this->events_ != nullptr))
    return true;
  RAMAllocator<Event> allocator(RAMAllocator<Event>::ALLOC_EXTERNAL);
  if (!deferred) {
    this->deliver_events();
    Event *events = this->events_;
    portENTER_CRITICAL(&this->event_mux_);
    this->events_ = nullptr;
    this->event_head_ = this->event_tail_ = 0;
    portEXIT_CRITICAL(&this->event_mux_);
    allocator.deallocate(events, EVENT_SLOTS);
    return true;
  }
  Event *events = allocator.allocate(EVENT_SLOTS);
  if (events == nullptr)
    return false;
  portENTER_CRITICAL(&this->event_mux_);
  this->event_head_ = this->event_tail_ = 0;
  this->events_ = events;
  portEXIT_CRITICAL(&this->event_mux_);
  return true;
#else
  return !deferred;
#endif
}

void HIDInput::push_event_(const Event &event) {
#ifdef USE_ESP32
  bool first = false;
  portENTER_CRITICAL(&this->event_mux_);
  if (this->events_ == nullptr || static_cast<uint8_t>(this->event_tail_ - this->event_head_) >= EVENT_SLOTS) {
    if (this->events_ != nullptr)
      this->events_dropped_++;
  } else {
    first = this->event_tail_ == this->event_head_;
    this->events_[this->event_tail_ % EVENT_SLOTS] = event;
    this->event_tail_++;
  }
  portEXIT_CRITICAL(&this->event_mux_);
  if (first)
    App.wake_loop_threadsafe();
#endif
}

void HIDInput::deliver_events() {
#ifdef USE_ESP32
  while (true) {
    Event e;
    portENTER_CRITICAL(&this->event_mux_);
    if (this->events_ == nullptr || this->event_head_ == this->event_tail_) {
      portEXIT_CRITICAL(&this->event_mux_);
      break;
    }
    e = this->events_[this->event_head_ % EVENT_SLOTS];
    this->event_head_++;
    portEXIT_CRITICAL(&this->event_mux_);
    switch (e.kind) {
      case EVENT_KEY:
        this->key_source_ = e.source;
        this->key_source_label_ = e.label;
        this->key_cb_.call(e.page, e.usage, e.flag != 0);
        break;
      case EVENT_TEXT:
        this->text_cb_.call(std::string(e.text, e.flag));
        break;
      case EVENT_POINTER:
        this->pointer_cb_.call(e.motion[0], e.motion[1], e.motion[2], e.motion[3], e.flag);
        break;
      case EVENT_CONTROL:
        this->control_cb_.call(e.page, e.usage, e.value);
        break;
      case EVENT_SEND_KEY:
        this->send_key_(e.flag);
        break;
    }
  }
  if (this->events_dropped_ != 0) {
    ESP_LOGW(TAG, "[%s] %u input events dropped: the main loop fell behind", this->log_name_,
             static_cast<unsigned>(this->events_dropped_));
    this->events_dropped_ = 0;
  }
#endif
}

void HIDInput::emit_key_(uint16_t page, uint16_t usage, bool pressed, uint32_t source, const char *label) {
  if (this->events_ == nullptr) {
    this->key_source_ = source;
    this->key_source_label_ = label;
    this->key_cb_.call(page, usage, pressed);
    return;
  }
  Event e{};
  e.kind = EVENT_KEY;
  e.flag = pressed ? 1 : 0;
  e.page = page;
  e.usage = usage;
  e.source = source;
  e.label = label;
  this->push_event_(e);
}

void HIDInput::emit_text_(const std::string &text) {
  if (this->events_ == nullptr) {
    this->text_cb_.call(text);
    return;
  }
  Event e{};
  e.kind = EVENT_TEXT;
  const size_t n = std::min(text.size(), sizeof(e.text));
  memcpy(e.text, text.data(), n);
  e.flag = static_cast<uint8_t>(n);
  this->push_event_(e);
}

void HIDInput::emit_pointer_(int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
  if (this->events_ == nullptr) {
    this->pointer_cb_.call(dx, dy, wheel, pan, buttons);
    return;
  }
  Event e{};
  e.kind = EVENT_POINTER;
  e.flag = buttons;
  e.motion[0] = dx;
  e.motion[1] = dy;
  e.motion[2] = wheel;
  e.motion[3] = pan;
  this->push_event_(e);
}

void HIDInput::emit_control_(uint16_t page, uint16_t usage, int32_t value) {
  if (this->events_ == nullptr) {
    this->control_cb_.call(page, usage, value);
    return;
  }
  Event e{};
  e.kind = EVENT_CONTROL;
  e.page = page;
  e.usage = usage;
  e.value = value;
  this->push_event_(e);
}

void HIDInput::emit_send_key_(uint8_t key) {
  if (this->events_ == nullptr) {
    this->send_key_(key);
    return;
  }
  Event e{};
  e.kind = EVENT_SEND_KEY;
  e.flag = key;
  this->push_event_(e);
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
          this->emit_control_(page, usage, static_cast<int32_t>(v.value));
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
      this->key_(page, this->pad_ != nullptr ? this->pad_->button(usage) : usage, v.value != 0);
      continue;
    }
    if (v.application_usage.page == PAGE_DIGITIZER && this->touch_(v))
      continue;
    if (page == PAGE_GENERIC_DEVICE && usage == USAGE_BATTERY_STRENGTH && v.logical_max > v.logical_min) {
      const int64_t percent = (v.value - v.logical_min) * 100 / (v.logical_max - v.logical_min);
      this->set_battery(static_cast<int8_t>(std::clamp<int64_t>(percent, 0, 100)), this->details_.battery_state);
      continue;
    }
    if (!v.is_relative && v.logical_min != v.logical_max) {
      if (page == PAGE_GENERIC_DESKTOP && usage == USAGE_HAT_SWITCH && pad_application(v.application_usage)) {
        this->gamepad_hat_(v);
        continue;
      }
      if (this->gamepad_axis_(v))
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
    this->emit_control_(page, usage, static_cast<int32_t>(v.value));
  }
  if (this->touch_changed_) {
    this->touch_changed_ = false;
    if (this->touch_down_ && this->touch_was_down_) {
      dx += this->touch_x_ - this->touch_last_x_;
      dy += this->touch_y_ - this->touch_last_y_;
    }
    if (this->touch_down_ != this->touch_was_down_) {
      this->buttons_ = this->touch_down_ ? (this->buttons_ | 0x01) : (this->buttons_ & ~0x01);
      this->key_(PAGE_BUTTON, 1, this->touch_down_);
    }
    this->touch_was_down_ = this->touch_down_;
    this->touch_last_x_ = this->touch_x_;
    this->touch_last_y_ = this->touch_y_;
    pointer = true;
  }
  if (pointer) {
    auto clamp16 = [](int32_t x) { return static_cast<int16_t>(std::clamp<int32_t>(x, INT16_MIN, INT16_MAX)); };
    this->emit_pointer_(clamp16(dx), clamp16(dy), clamp16(wheel), clamp16(pan), this->buttons_);
  }
}

// Tip switch and absolute X/Y of the first contact (the first finger collection); the
// other contacts' are not reported.
bool HIDInput::touch_(const HIDReportItemValue &v) {
  const uint16_t app = v.application_usage.usage;
  if (app != 0x01 && app != 0x02 && app != 0x04 && app != 0x05)  // digitizer, pen, touch screen, touch pad
    return false;
  const bool tip = v.usage.page == PAGE_DIGITIZER && v.usage.usage == 0x42;
  const bool axis = v.usage.page == PAGE_GENERIC_DESKTOP && !v.is_relative &&
                    (v.usage.usage == USAGE_X || v.usage.usage == USAGE_Y);
  if (!tip && !axis)
    return false;
  if (this->touch_collection_ == HIDCollectionSchema::NO_PARENT || v.collection_id < this->touch_collection_)
    this->touch_collection_ = v.collection_id;
  if (v.collection_id != this->touch_collection_)
    return true;
  if (tip) {
    this->touch_down_ = v.value != 0;
  } else if (v.usage.usage == USAGE_X) {
    this->touch_x_ = static_cast<int32_t>(v.value);
  } else {
    this->touch_y_ = static_cast<int32_t>(v.value);
  }
  this->touch_changed_ = true;
  return true;
}

void HIDInput::release_all_hid() {
  uint32_t down[MAX_DOWN];
  const uint8_t count = this->down_count_;
  memcpy(down, this->down_, count * sizeof(down[0]));
  for (uint8_t i = count; i > 0; i--)
    this->key_(down[i - 1] >> 16, down[i - 1] & 0xFFFF, false);
  if (this->buttons_ != 0) {
    this->buttons_ = 0;
    this->emit_pointer_(0, 0, 0, 0, 0);
  }
  this->modifiers_ = 0;
  this->dead_ = 0;
  this->touch_down_ = this->touch_was_down_ = this->touch_changed_ = false;
  this->touch_collection_ = HIDCollectionSchema::NO_PARENT;
  this->details_ = HIDDeviceDetails{};
}

void HIDInput::set_firmware(const char *text) {
  snprintf(this->details_.firmware, sizeof(this->details_.firmware), "%s", text);
}

void HIDInput::set_battery(int8_t percent, HIDBattery state) {
  const bool changed = percent != this->details_.battery;
  this->details_.battery = percent;
  this->details_.battery_state = state;
  if (changed && percent >= 0)
    this->emit_control_(PAGE_GENERIC_DEVICE, USAGE_BATTERY_STRENGTH, percent);
}

void HIDInput::set_signal(int8_t value, bool delta) {
  this->details_.has_signal = true;
  this->details_.signal_delta = delta;
  this->details_.signal = value;
  this->emit_control_(PAGE_GENERIC_DEVICE, USAGE_SIGNAL, value);
}

void HIDInput::key_(uint16_t page, uint16_t usage, bool pressed) {
  const uint32_t key = static_cast<uint32_t>(page) << 16 | usage;
  uint8_t at = 0;
  while (at < this->down_count_ && this->down_[at] != key)
    at++;
  if (pressed == (at < this->down_count_))
    return;
  if (pressed) {
    if (this->down_count_ >= MAX_DOWN)
      return;
    this->down_[this->down_count_] = key;
    this->down_count_++;
  } else {
    for (uint8_t i = at; i + 1 < this->down_count_; i++)
      this->down_[i] = this->down_[i + 1];
    this->down_count_--;
  }
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  if (this->log_name_[0] != '\0')
    ESP_LOGD(TAG, "[%s] %s %s", this->log_name_, key_name(page, usage).c_str(), pressed ? "pressed" : "released");
#endif
  if (page == PAGE_KEYBOARD && usage >= KEY_LEFT_CTRL && usage <= KEY_RIGHT_GUI) {
    const uint8_t bit = 1u << (usage - KEY_LEFT_CTRL);
    this->modifiers_ = pressed ? (this->modifiers_ | bit) : (this->modifiers_ & ~bit);
  }
  const uint32_t source =
      this->forward_source_ != 0 ? this->forward_source_ : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
  const char *label = this->forward_label_ != nullptr ? this->forward_label_ : this->device_label_();
  DirectKeySink *sink = direct_sink.load(std::memory_order_acquire);
  if (sink != nullptr)
    sink->direct_key(source, page, usage, pressed);
  this->emit_key_(page, usage, pressed, source, label);
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
    this->emit_send_key_('\b');
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
  const LayoutKey *key = this->layout_key_(usage);
  if (key == nullptr)
    return;
  const uint8_t level = this->level_(*key);
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

const LayoutKey *HIDInput::layout_key_(uint16_t usage) const {
  const KeyboardLayout *layout = this->active_layout_();
  if (layout == nullptr)
    return nullptr;
  for (uint16_t i = 0; i < layout->key_count; i++) {
    if (layout->keys[i].usage == usage)
      return &layout->keys[i];
  }
  return nullptr;
}

uint8_t HIDInput::level_(const LayoutKey &key) const {
  bool shift = (this->modifiers_ & (MOD_LEFT_SHIFT | MOD_RIGHT_SHIFT)) != 0;
  if (key.alphabetic && (this->leds_ & LED_CAPS_LOCK) != 0)
    shift = !shift;
  return ((this->modifiers_ & MOD_RIGHT_ALT) != 0 ? 2 : 0) + (shift ? 1 : 0);
}

char32_t HIDInput::layout_char(uint16_t usage) const {
  const LayoutKey *key = this->layout_key_(usage);
  if (key == nullptr)
    return 0;
  const uint8_t level = this->level_(*key);
  const char32_t c = key->chars[level];
  return c != 0 && (key->dead & (1u << level)) != 0 ? (LAYOUT_DEAD | c) : c;
}

bool HIDInput::layout_has_level3() const {
  const KeyboardLayout *layout = this->active_layout_();
  if (layout == nullptr)
    return false;
  for (uint16_t i = 0; i < layout->key_count; i++) {
    if (layout->keys[i].chars[2] != 0 || layout->keys[i].chars[3] != 0)
      return true;
  }
  return false;
}

char32_t HIDInput::compose_(char32_t mark, char32_t base) const {
  const KeyboardLayout *layout = this->active_layout_();
  if (layout == nullptr)
    return 0;
  for (uint16_t i = 0; i < layout->compose_count; i++) {
    const auto &entry = layout->compose[i];
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
          this->emit_send_key_(static_cast<uint8_t>(alone));
      }
    }
  }
  append_utf8(text, c);
  if (c < 0x80)
    this->emit_send_key_(static_cast<uint8_t>(c));
  this->emit_text_(text);
}

}  // namespace esphome::hid
