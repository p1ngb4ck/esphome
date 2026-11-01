#include "boot_report_decoder.h"

#include <utility>

namespace esphome {
namespace hid {

void HIDBootReportDecoder::reset() {
  this->keyboard_modifiers_.clear();
  this->keyboard_keys_.clear();
  this->mouse_buttons_.clear();
}

std::vector<HIDReportItemValue> HIDBootReportDecoder::decode(const HIDReportSource &source, const uint8_t *data,
                                                             size_t length, HIDDecodeStatus *status) {
  if (status == nullptr)
    return {};
  if (data == nullptr && length != 0) {
    *status = HIDDecodeStatus::INVALID_PAYLOAD;
    return {};
  }

  std::vector<HIDReportItemValue> values;
  const uint16_t handle = source.characteristic_handle;
  if (source.characteristic_uuid == BOOT_MOUSE_INPUT_UUID) {
    if (length < 3) {
      *status = HIDDecodeStatus::SHORT;
      return {};
    }
    *status = length == 3 ? HIDDecodeStatus::EXACT : HIDDecodeStatus::LONG;
    uint8_t &previous = this->mouse_buttons_[handle];
    for (uint8_t bit = 0; bit < 3; bit++) {
      const bool active = (data[0] & (1U << bit)) != 0;
      const bool was_active = (previous & (1U << bit)) != 0;
      if (active != was_active) {
        HIDReportItemValue value(HIDUsage(bit + 1, 0x09), active ? 1 : 0, active ? 1 : 0);
        value.field_id = 0xF000U + bit;
        value.application_usage = HIDUsage(0x02, 0x01);
        values.push_back(value);
      }
    }
    previous = data[0];
    const int8_t x = static_cast<int8_t>(data[1]);
    const int8_t y = static_cast<int8_t>(data[2]);
    if (x != 0) {
      HIDReportItemValue value(HIDUsage(0x30, 0x01), x, x);
      value.field_id = 0xF010;
      value.application_usage = HIDUsage(0x02, 0x01);
      value.is_relative = true;
      value.aggregation = HIDReportItemValue::Aggregation::SUM;
      values.push_back(value);
    }
    if (y != 0) {
      HIDReportItemValue value(HIDUsage(0x31, 0x01), y, y);
      value.field_id = 0xF011;
      value.application_usage = HIDUsage(0x02, 0x01);
      value.is_relative = true;
      value.aggregation = HIDReportItemValue::Aggregation::SUM;
      values.push_back(value);
    }
    return values;
  }

  if (source.characteristic_uuid == BOOT_KEYBOARD_INPUT_UUID) {
    if (length < 8) {
      *status = HIDDecodeStatus::SHORT;
      return {};
    }
    *status = length == 8 ? HIDDecodeStatus::EXACT : HIDDecodeStatus::LONG;
    auto &modifiers = this->keyboard_modifiers_[handle];
    for (uint8_t bit = 0; bit < 8; bit++) {
      const bool active = (data[0] & (1U << bit)) != 0;
      if (modifiers[bit] != active) {
        HIDReportItemValue value(HIDUsage(static_cast<uint16_t>(0xE0 + bit), 0x07), active ? 1 : 0, active ? 1 : 0);
        value.field_id = 0xF100U + bit;
        value.application_usage = HIDUsage(0x06, 0x01);
        values.push_back(value);
        modifiers[bit] = active;
      }
    }
    std::set<uint8_t> current;
    for (size_t i = 2; i < 8; i++) {
      if (data[i] != 0 && data[i] <= 0xE7)
        current.insert(data[i]);
    }
    auto &previous = this->keyboard_keys_[handle];
    for (uint8_t key : current) {
      if (previous.count(key) == 0) {
        HIDReportItemValue value(HIDUsage(key, 0x07), 1, key);
        value.field_id = 0xF200;
        value.application_usage = HIDUsage(0x06, 0x01);
        values.push_back(value);
      }
    }
    for (uint8_t key : previous) {
      if (current.count(key) == 0) {
        HIDReportItemValue value(HIDUsage(key, 0x07), 0, 0);
        value.field_id = 0xF200;
        value.application_usage = HIDUsage(0x06, 0x01);
        values.push_back(value);
      }
    }
    previous = std::move(current);
    return values;
  }

  *status = HIDDecodeStatus::BOOT_REPORT;
  return {};
}

}  // namespace hid
}  // namespace esphome
