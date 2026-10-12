#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "hid_parser.h"

namespace esphome {
namespace hid {

class HIDBootReportDecoder {
 public:
  static constexpr uint16_t BOOT_KEYBOARD_INPUT_UUID = 0x2A22;
  static constexpr uint16_t BOOT_MOUSE_INPUT_UUID = 0x2A33;

  std::vector<HIDReportItemValue> decode(const HIDReportSource &source, const uint8_t *data, size_t length,
                                         HIDDecodeStatus *status);
  void reset();

 private:
  std::map<uint16_t, std::array<bool, 8>> keyboard_modifiers_;
  std::map<uint16_t, std::set<uint8_t>> keyboard_keys_;
  std::map<uint16_t, uint8_t> mouse_buttons_;
};

}  // namespace hid
}  // namespace esphome
