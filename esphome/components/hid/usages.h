#pragma once

#include <cstdint>

namespace esphome {
namespace hid {

extern const char *const HID_USAGE_TABLE_VERSION;
const char *lookup_usage_page_name(uint16_t page);
const char *lookup_usage_name(uint16_t page, uint16_t usage);
const char *lookup_usage_kinds(uint16_t page, uint16_t usage);

}  // namespace hid
}  // namespace esphome
