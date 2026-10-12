#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"

namespace esphome::hid {

// Emitted by codegen in a firmware; a few entries are enough here.
extern const KeyName KEY_NAMES[];
const KeyName KEY_NAMES[] = {{0x07, 0x04, "A"}, {0x0C, 0xE9, "VOLUME_UP"}};
const uint16_t KEY_NAMES_COUNT = 2;

namespace testing {

// a/A/æ/Æ, a dead acute (plain) / dead grave (Shift), and e/E.
static const LayoutKey KEYS[] = {
    {0x04, 0x0, true, {U'a', U'A', U'æ', U'Æ'}},
    {0x2E, 0x3, false, {0x0301, 0x0300, 0, 0}},
    {0x08, 0x0, true, {U'e', U'E', U'€', 0}},
};
static const LayoutCompose COMPOSE[] = {
    {0x0301, U' ', U'´'}, {0x0301, 0x0301, U'´'}, {0x0301, U'e', U'é'},
    {0x0301, U'E', U'É'}, {0x0300, U' ', U'`'},   {0x0300, U'e', U'è'},
};
static const KeyboardLayout LAYOUT = {KEYS, 3, COMPOSE, 6};

class TestInput : public HIDInput {
 public:
  TestInput() {
    this->set_layout(&LAYOUT);
    this->add_on_text_callback([this](const std::string &text) { this->text += text; });
    this->add_on_key_event_callback([this](uint16_t page, uint16_t usage, bool pressed) {
      if (pressed)
        this->keys.push_back(key_name(page, usage));
    });
    this->add_on_key_callback([this](uint8_t key) { this->provided += static_cast<char>(key); });
  }
  void key(uint16_t usage, bool pressed, uint16_t page = PAGE_KEYBOARD) {
    HIDReportItemValue v(HIDUsage(usage, page), pressed ? 1 : 0, pressed ? 1 : 0);
    this->process_hid_values({v});
  }
  void tap(uint16_t usage) {
    this->key(usage, true);
    this->key(usage, false);
  }
  std::string text;
  std::string provided;
  std::vector<std::string> keys;
};

TEST(HIDInput, PlainShiftAndCapsLock) {
  TestInput in;
  in.tap(0x04);
  in.key(0xE1, true);
  in.tap(0x04);
  in.key(0xE1, false);
  in.tap(0x39);  // Caps Lock
  in.tap(0x04);
  in.key(0xE5, true);  // Shift with Caps Lock: lower case again
  in.tap(0x04);
  EXPECT_EQ(in.text, "aAAa");
  EXPECT_EQ(in.get_leds() & LED_CAPS_LOCK, LED_CAPS_LOCK);
}

TEST(HIDInput, AltGrAndShortcuts) {
  TestInput in;
  in.key(0xE6, true);  // AltGr
  in.tap(0x04);
  in.tap(0x08);
  in.key(0xE6, false);
  in.key(0xE0, true);  // Ctrl+A types nothing
  in.tap(0x04);
  EXPECT_EQ(in.text, "æ€");
}

TEST(HIDInput, DeadKeys) {
  TestInput in;
  in.tap(0x2E);
  in.tap(0x08);  // ´ + e
  in.tap(0x2E);
  in.tap(0x2C);  // ´ + space
  in.tap(0x2E);
  in.tap(0x2E);  // ´ twice
  in.tap(0x2E);
  in.tap(0x04);  // ´ + a: nothing composed, both typed
  EXPECT_EQ(in.text, "é´´´a");
}

TEST(HIDInput, KeypadAndKeyProvider) {
  TestInput in;
  in.tap(0x59);  // KP 1 with Num Lock on
  in.tap(0x53);  // Num Lock off
  in.tap(0x59);
  in.tap(0x28);  // Enter
  in.tap(0x2A);  // Backspace
  EXPECT_EQ(in.text, "1\n");
  EXPECT_EQ(in.provided, "1\n\b");
}

TEST(HIDInput, KeyNamesAndRelease) {
  TestInput in;
  in.key(0xE9, true, PAGE_CONSUMER);
  in.key(0x05, true);
  EXPECT_EQ(in.keys, (std::vector<std::string>{"VOLUME_UP", "0x07:0x05"}));
  EXPECT_TRUE(in.is_key_down(PAGE_CONSUMER, 0xE9));
  in.release_all_hid();
  EXPECT_FALSE(in.is_key_down(PAGE_CONSUMER, 0xE9));
  EXPECT_FALSE(in.is_key_down(PAGE_KEYBOARD, 0x05));
}

// HID 1.11 appendix B: the boot keyboard and boot mouse report descriptors.
static const uint8_t BOOT_KEYBOARD[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};
static const uint8_t BOOT_MOUSE[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15,
    0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};

TEST(HIDInput, KeyboardThroughTheParser) {
  std::unique_ptr<HIDReportMap> map(HIDReportMap::parse_report_map_data(BOOT_KEYBOARD, sizeof(BOOT_KEYBOARD)));
  ASSERT_NE(map, nullptr);
  TestInput in;
  HIDReportSource source;
  const uint8_t shift_a[8] = {0x02, 0, 0x04, 0, 0, 0, 0, 0};
  const uint8_t none[8] = {};
  in.process_hid_values(map->parse(source, shift_a, sizeof(shift_a)));
  in.process_hid_values(map->parse(source, none, sizeof(none)));
  EXPECT_EQ(in.text, "A");
  EXPECT_EQ(in.get_modifiers(), 0);
}

TEST(HIDInput, MouseThroughTheParser) {
  std::unique_ptr<HIDReportMap> map(HIDReportMap::parse_report_map_data(BOOT_MOUSE, sizeof(BOOT_MOUSE)));
  ASSERT_NE(map, nullptr);
  TestInput in;
  int dx = 0, dy = 0, buttons = -1;
  in.add_on_pointer_callback([&](int16_t x, int16_t y, int16_t, int16_t, uint8_t b) {
    dx += x;
    dy += y;
    buttons = b;
  });
  HIDReportSource source;
  const uint8_t report[3] = {0x01, 0x05, 0xFD};
  in.process_hid_values(map->parse(source, report, sizeof(report)));
  EXPECT_EQ(dx, 5);
  EXPECT_EQ(dy, -3);
  EXPECT_EQ(buttons, 1);
}

}  // namespace testing
}  // namespace esphome::hid
