#pragma once

#include <cstdint>

namespace esphome {
namespace hid {

// HID short-item size code 3 represents four data bytes; the other codes are
// their literal byte counts.
constexpr uint8_t hid_item_data_size(uint8_t item_info) {
  const uint8_t encoded_size = item_info & 0x03;
  return encoded_size == 3 ? 4 : encoded_size;
}

constexpr int32_t decode_signed_hid_item(uint32_t value, uint8_t item_info) {
  const uint8_t byte_count = hid_item_data_size(item_info);
  if (byte_count == 0)
    return 0;

  const uint8_t bit_count = byte_count * 8;
  const uint32_t value_mask = bit_count == 32 ? UINT32_MAX : (uint32_t{1} << bit_count) - 1;
  const uint32_t sign_bit = uint32_t{1} << (bit_count - 1);
  value &= value_mask;
  if ((value & sign_bit) != 0)
    return static_cast<int32_t>(static_cast<int64_t>(value) - (int64_t{1} << bit_count));
  return static_cast<int32_t>(value);
}

}  // namespace hid
}  // namespace esphome
