#include "hid_parser.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>

#include "esphome/core/log.h"
#include "hid_item_value.h"
#include "hid_report_data.h"

namespace esphome {
namespace hid {

static const char *const TAG = "hid_parser";

namespace {

std::string format_string(const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int length = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (length < 0) {
    va_end(args);
    return {};
  }
  std::vector<char> buffer(static_cast<size_t>(length) + 1U);
  std::vsnprintf(buffer.data(), buffer.size(), format, args);
  va_end(args);
  return std::string(buffer.data(), static_cast<size_t>(length));
}

uint32_t read_unsigned_item(const uint8_t *data, uint8_t size) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < size; i++)
    value |= static_cast<uint32_t>(data[i]) << (8U * i);
  return value;
}

int64_t signed_or_unsigned_max(uint32_t raw, uint8_t item_info, int64_t minimum) {
  return minimum < 0 ? decode_signed_hid_item(raw, item_info) : static_cast<int64_t>(raw);
}

int8_t decode_unit_exponent(uint32_t raw, uint8_t item_size) {
  // HID 1.11 encodes a one-nibble signed exponent even when carried in a
  // wider short item. Preserve only that defined nibble.
  (void) item_size;
  uint8_t nibble = static_cast<uint8_t>(raw & 0x0FU);
  return (nibble & 0x08U) != 0 ? static_cast<int8_t>(nibble | 0xF0U) : static_cast<int8_t>(nibble);
}

HIDUsage decode_usage(uint32_t raw, uint8_t size, uint16_t current_page) {
  if (size == 4)
    return HIDUsage(static_cast<uint16_t>(raw), static_cast<uint16_t>(raw >> 16U));
  return HIDUsage(static_cast<uint16_t>(raw), current_page);
}

struct GlobalState {
  uint16_t usage_page{0};
  HIDRange logical{};
  HIDRange physical{};
  int8_t unit_exponent{0};
  uint32_t unit{0};
  uint32_t report_size{0};
  uint32_t report_count{0};
  uint8_t report_id{0};
};

struct LocalState {
  std::vector<std::vector<HIDUsage>> usage_sets{{}};
  size_t active_usage_set{0};
  HIDUsage usage_minimum{};
  HIDUsage usage_maximum{};
  bool has_usage_minimum{false};
  bool has_usage_maximum{false};
  std::vector<uint32_t> designators;
  uint32_t designator_minimum{0};
  uint32_t designator_maximum{0};
  bool has_designator_minimum{false};
  bool has_designator_maximum{false};
  std::vector<uint32_t> strings;
  uint32_t string_minimum{0};
  uint32_t string_maximum{0};
  bool has_string_minimum{false};
  bool has_string_maximum{false};
  bool delimiter_open{false};

  std::vector<HIDUsage> &active_usages() { return this->usage_sets[this->active_usage_set]; }
  void reset() { *this = LocalState{}; }
};

bool is_input_kind(uint8_t report_type) { return report_type == static_cast<uint8_t>(HIDReportKind::INPUT); }

}  // namespace

const char *hid_report_kind_name(HIDReportKind kind) {
  switch (kind) {
    case HIDReportKind::INPUT:
      return "input";
    case HIDReportKind::OUTPUT:
      return "output";
    case HIDReportKind::FEATURE:
      return "feature";
  }
  return "unknown";
}

const char *hid_decode_status_name(HIDDecodeStatus status) {
  switch (status) {
    case HIDDecodeStatus::EXACT:
      return "exact";
    case HIDDecodeStatus::LONG:
      return "long";
    case HIDDecodeStatus::SHORT:
      return "short";
    case HIDDecodeStatus::SCHEMA_MISSING:
      return "schema_missing";
    case HIDDecodeStatus::REPORT_KIND_MISMATCH:
      return "report_kind_mismatch";
    case HIDDecodeStatus::BOOT_REPORT:
      return "boot_report";
    case HIDDecodeStatus::INVALID_PAYLOAD:
      return "invalid_payload";
  }
  return "unknown";
}

HIDUsage HIDFieldSchema::usage_at(uint32_t index, bool repeat_last) const {
  if (index < this->usages.size())
    return this->usages[index];
  if (repeat_last && !this->usages.empty())
    return this->usages.back();
  if (this->has_usage_range && this->usage_minimum.page == this->usage_maximum.page &&
      this->usage_maximum.usage >= this->usage_minimum.usage &&
      index <= static_cast<uint32_t>(this->usage_maximum.usage - this->usage_minimum.usage))
    return HIDUsage(static_cast<uint16_t>(this->usage_minimum.usage + index), this->usage_minimum.page);
  return {};
}

std::string HIDReportItemValue::to_string() const {
  return format_string("HIDReportItemValue(field=%u usage_page=%u usage=%u value=%" PRId64 ")", this->field_id,
                       this->usage.page, this->usage.usage, this->value);
}

void HIDReportMap::add_diagnostic_(size_t offset, bool error, std::string message) {
  this->diagnostics_.push_back({offset, error, std::move(message)});
  if (error)
    this->valid_ = false;
}

const HIDReportSchema *HIDReportMap::find_report(HIDReportKind kind, uint8_t report_id) const {
  for (const auto &report : this->reports_)
    if (report.kind == kind && report.report_id == report_id)
      return &report;
  return nullptr;
}

const HIDFieldSchema *HIDReportMap::find_field(uint16_t field_id) const {
  return field_id < this->fields_.size() ? &this->fields_[field_id] : nullptr;
}

HIDUsage HIDReportMap::application_usage(uint16_t collection_id) const {
  uint16_t current = collection_id;
  while (current != HIDCollectionSchema::NO_PARENT && current < this->collections_.size()) {
    const auto &collection = this->collections_[current];
    if (collection.type == 0x01)
      return collection.usage;
    current = collection.parent;
  }
  return {};
}

uint8_t HIDReportMap::device_kinds() const {
  static constexpr uint16_t GENERIC_DESKTOP = 0x01;
  static constexpr uint16_t CONSUMER = 0x0C;
  static constexpr uint16_t DIGITIZER = 0x0D;
  static constexpr uint16_t KEYBOARD_PAGE = 0x07;
  static constexpr uint16_t KEY_A = 0x04;
  static constexpr uint16_t KEY_Z = 0x1D;
  // A keyboard collection without letter keys (a remote's keys sent as keyboard usages) is not a keyboard.
  bool letters = false;
  for (const auto &field : this->fields_) {
    if (letters)
      break;
    if (field.kind != HIDReportKind::INPUT || field.is_constant())
      continue;
    const HIDUsage application = this->application_usage(field.collection_id);
    if (application.page != GENERIC_DESKTOP || application.usage != 0x06)
      continue;
    if (field.has_usage_range && field.usage_minimum.page == KEYBOARD_PAGE && field.usage_minimum.usage <= KEY_Z &&
        field.usage_maximum.usage >= KEY_A) {
      letters = true;
      break;
    }
    for (const auto &usage : field.usages) {
      if (usage.page == KEYBOARD_PAGE && usage.usage >= KEY_A && usage.usage <= KEY_Z) {
        letters = true;
        break;
      }
    }
  }
  uint8_t kinds = 0;
  bool controls = false;
  for (const auto &collection : this->collections_) {
    if (collection.type != 0x01)
      continue;
    const HIDUsage &usage = collection.usage;
    if (usage.page == GENERIC_DESKTOP) {
      switch (usage.usage) {
        case 0x06:  // keyboard
          if (letters) {
            kinds |= HID_DEVICE_KEYBOARD;
          } else {
            controls = true;
          }
          break;
        case 0x07:  // keypad
          kinds |= HID_DEVICE_KEYBOARD;
          break;
        case 0x01:  // pointer
        case 0x02:  // mouse
          kinds |= HID_DEVICE_MOUSE;
          break;
        case 0x04:  // joystick
        case 0x05:  // gamepad
        case 0x08:  // multi-axis controller
          kinds |= HID_DEVICE_GAMEPAD;
          break;
        case 0x80:  // system control
          controls = true;
          break;
        default:
          break;
      }
    } else if (usage.page == DIGITIZER && (usage.usage == 0x04 || usage.usage == 0x05)) {
      kinds |= HID_DEVICE_MOUSE;  // touch screen, touch pad
    } else if (usage.page == CONSUMER && usage.usage == 0x01) {
      controls = true;
    }
  }
  if (kinds == 0 && controls)
    kinds = HID_DEVICE_REMOTE;
  return kinds;
}

void HIDReportMap::reset_runtime_state() { this->runtime_.clear(); }

HIDReportMap *HIDReportMap::parse_report_map_data(const uint8_t *data, size_t length) {
  if (data == nullptr || length == 0 || length > MAX_DESCRIPTOR_BYTES)
    return nullptr;

  auto *map = new HIDReportMap();
  GlobalState global;
  LocalState local;
  std::vector<GlobalState> global_stack;
  std::vector<uint16_t> collection_stack;
  size_t offset = 0;

  auto fail = [&](size_t item_offset, const char *message) -> HIDReportMap * {
    map->add_diagnostic_(item_offset, true, message);
    delete map;
    return nullptr;
  };

  auto find_or_add_report = [&](HIDReportKind kind, uint8_t report_id) -> HIDReportSchema * {
    for (auto &report : map->reports_)
      if (report.kind == kind && report.report_id == report_id)
        return &report;
    if (map->reports_.size() >= MAX_REPORTS)
      return nullptr;
    map->reports_.push_back({kind, report_id, 0, {}});
    return &map->reports_.back();
  };

  while (offset < length) {
    const size_t item_offset = offset;
    const uint8_t prefix = data[offset++];
    if (prefix == 0xFE) {
      if (length - offset < 2)
        return fail(item_offset, "truncated long-item header");
      const uint8_t payload_size = data[offset++];
      const uint8_t tag = data[offset++];
      if (length - offset < payload_size)
        return fail(item_offset, "truncated long-item payload");
      map->add_diagnostic_(item_offset, false,
                           format_string("preserved long item tag=0x%02X size=%u", tag, payload_size));
      offset += payload_size;
      continue;
    }

    const uint8_t item_size = hid_item_data_size(prefix);
    if (length - offset < item_size)
      return fail(item_offset, "truncated short item");
    const uint32_t raw = read_unsigned_item(data + offset, item_size);
    offset += item_size;
    const uint8_t tag = prefix & (HID_ITEM_TYPE_MASK | HID_ITEM_TAG_MASK);

    switch (tag) {
      case HID_ITEM_TYPE_TAG_USAGE_PAGE:
        if (raw > UINT16_MAX)
          return fail(item_offset, "usage page exceeds 16 bits");
        global.usage_page = static_cast<uint16_t>(raw);
        break;
      case HID_ITEM_TYPE_TAG_LOGICAL_MINIMUM:
        global.logical.minimum = decode_signed_hid_item(raw, prefix);
        break;
      case HID_ITEM_TYPE_TAG_LOGICAL_MAXIMUM:
        global.logical.maximum = signed_or_unsigned_max(raw, prefix, global.logical.minimum);
        break;
      case HID_ITEM_TYPE_TAG_PHYSICAL_MINIMUM:
        global.physical.minimum = decode_signed_hid_item(raw, prefix);
        break;
      case HID_ITEM_TYPE_TAG_PHYSICAL_MAXIMUM:
        global.physical.maximum = signed_or_unsigned_max(raw, prefix, global.physical.minimum);
        break;
      case HID_ITEM_TYPE_TAG_UNIT_EXPONENT:
        global.unit_exponent = decode_unit_exponent(raw, item_size);
        break;
      case HID_ITEM_TYPE_TAG_UNIT:
        global.unit = raw;
        break;
      case HID_ITEM_TYPE_TAG_REPORT_SIZE:
        global.report_size = raw;
        break;
      case HID_ITEM_TYPE_TAG_REPORT_COUNT:
        global.report_count = raw;
        break;
      case HID_ITEM_TYPE_TAG_REPORT_ID:
        if (raw == 0 || raw > UINT8_MAX)
          return fail(item_offset, "invalid Report ID");
        global.report_id = static_cast<uint8_t>(raw);
        map->uses_report_ids_ = true;
        break;
      case HID_ITEM_TYPE_TAG_PUSH:
        if (global_stack.size() >= MAX_GLOBAL_STACK_DEPTH)
          return fail(item_offset, "global stack limit exceeded");
        global_stack.push_back(global);
        break;
      case HID_ITEM_TYPE_TAG_POP:
        if (global_stack.empty())
          return fail(item_offset, "global Pop without Push");
        global = global_stack.back();
        global_stack.pop_back();
        break;
      case HID_ITEM_TYPE_TAG_USAGE:
        local.active_usages().push_back(decode_usage(raw, item_size, global.usage_page));
        break;
      case HID_ITEM_TYPE_TAG_USAGE_MINIMUM:
        local.usage_minimum = decode_usage(raw, item_size, global.usage_page);
        local.has_usage_minimum = true;
        break;
      case HID_ITEM_TYPE_TAG_USAGE_MAXIMUM:
        local.usage_maximum = decode_usage(raw, item_size, global.usage_page);
        local.has_usage_maximum = true;
        break;
      case 0x38:  // Designator Index
        local.designators.push_back(raw);
        break;
      case 0x48:  // Designator Minimum
        local.designator_minimum = raw;
        local.has_designator_minimum = true;
        break;
      case 0x58:  // Designator Maximum
        local.designator_maximum = raw;
        local.has_designator_maximum = true;
        break;
      case 0x78:  // String Index
        local.strings.push_back(raw);
        break;
      case 0x88:  // String Minimum
        local.string_minimum = raw;
        local.has_string_minimum = true;
        break;
      case 0x98:  // String Maximum
        local.string_maximum = raw;
        local.has_string_maximum = true;
        break;
      case HID_ITEM_TYPE_TAG_DELIMITER:
        if (raw == 1) {
          if (local.delimiter_open)
            return fail(item_offset, "nested Delimiter");
          local.delimiter_open = true;
          local.usage_sets.push_back({});
          local.active_usage_set = local.usage_sets.size() - 1;
        } else if (raw == 0) {
          if (!local.delimiter_open)
            return fail(item_offset, "Delimiter close without open");
          local.delimiter_open = false;
          local.active_usage_set = 0;
        } else {
          return fail(item_offset, "invalid Delimiter value");
        }
        break;
      case HID_ITEM_TYPE_TAG_COLLECTION: {
        if (map->collections_.size() >= MAX_COLLECTIONS || collection_stack.size() >= MAX_COLLECTION_DEPTH)
          return fail(item_offset, "collection limit exceeded");
        HIDCollectionSchema collection;
        collection.id = static_cast<uint16_t>(map->collections_.size());
        collection.parent = collection_stack.empty() ? HIDCollectionSchema::NO_PARENT : collection_stack.back();
        collection.type = static_cast<uint8_t>(raw);
        if (!local.usage_sets[0].empty())
          collection.usage = local.usage_sets[0].front();
        else if (local.has_usage_minimum)
          collection.usage = local.usage_minimum;
        for (size_t set = 1; set < local.usage_sets.size(); set++)
          collection.aliases.insert(collection.aliases.end(), local.usage_sets[set].begin(),
                                    local.usage_sets[set].end());
        map->collections_.push_back(std::move(collection));
        collection_stack.push_back(static_cast<uint16_t>(map->collections_.size() - 1));
        local.reset();
        break;
      }
      case HID_ITEM_TYPE_TAG_END_COLLECTION:
        if (collection_stack.empty())
          return fail(item_offset, "End Collection without Collection");
        collection_stack.pop_back();
        local.reset();
        break;
      case HID_ITEM_TYPE_TAG_INPUT:
      case HID_ITEM_TYPE_TAG_OUTPUT:
      case HID_ITEM_TYPE_TAG_FEATURE: {
        const HIDReportKind kind = tag == HID_ITEM_TYPE_TAG_INPUT    ? HIDReportKind::INPUT
                                   : tag == HID_ITEM_TYPE_TAG_OUTPUT ? HIDReportKind::OUTPUT
                                                                     : HIDReportKind::FEATURE;
        if (global.report_size == 0 || global.report_size > 32 || global.report_count == 0 ||
            global.report_count > MAX_REPORT_COUNT)
          return fail(item_offset, "invalid report size or count");
        if (local.delimiter_open)
          return fail(item_offset, "unterminated Delimiter");
        HIDReportSchema *report = find_or_add_report(kind, global.report_id);
        if (report == nullptr || map->fields_.size() >= MAX_FIELDS)
          return fail(item_offset, "report or field limit exceeded");
        const uint64_t field_bits = static_cast<uint64_t>(global.report_size) * global.report_count;
        if (field_bits > MAX_REPORT_BITS || report->bit_size > MAX_REPORT_BITS - field_bits)
          return fail(item_offset, "report bit length limit exceeded");

        HIDFieldSchema field;
        field.id = static_cast<uint16_t>(map->fields_.size());
        field.kind = kind;
        field.report_id = global.report_id;
        field.collection_id = collection_stack.empty() ? HIDCollectionSchema::NO_PARENT : collection_stack.back();
        field.bit_offset = report->bit_size;
        field.report_size = global.report_size;
        field.report_count = global.report_count;
        field.flags = static_cast<uint16_t>(raw & 0x1FFU);
        field.logical = global.logical;
        field.physical = global.physical;
        field.unit_exponent = global.unit_exponent;
        field.unit = global.unit;
        field.usages = local.usage_sets[0];
        for (size_t set = 1; set < local.usage_sets.size(); set++)
          field.alternative_usages.push_back(local.usage_sets[set]);
        field.usage_minimum = local.usage_minimum;
        field.usage_maximum = local.usage_maximum;
        field.has_usage_range = local.has_usage_minimum && local.has_usage_maximum;
        field.string_indices = local.strings;
        field.string_minimum = local.string_minimum;
        field.string_maximum = local.string_maximum;
        field.has_string_range = local.has_string_minimum && local.has_string_maximum;
        field.designator_indices = local.designators;
        field.designator_minimum = local.designator_minimum;
        field.designator_maximum = local.designator_maximum;
        field.has_designator_range = local.has_designator_minimum && local.has_designator_maximum;

        report->field_ids.push_back(field.id);
        report->bit_size += static_cast<uint32_t>(field_bits);
        map->fields_.push_back(std::move(field));
        local.reset();
        break;
      }
      default:
        map->add_diagnostic_(item_offset, false,
                             format_string("preserved unknown item prefix=0x%02X value=0x%08X", prefix, raw));
        if ((prefix & HID_ITEM_TYPE_MASK) == HID_ITEM_TYPE_MAIN)
          local.reset();
        break;
    }
  }

  if (!collection_stack.empty()) {
    delete map;
    return nullptr;
  }
  if (!global_stack.empty())
    map->add_diagnostic_(length, false, "global Push stack not empty at end of descriptor");
  if (map->reports_.empty()) {
    delete map;
    return nullptr;
  }
  return map;
}

void HIDReportMap::esp_logd_report_map(const uint8_t *data, size_t length) {
  if (data == nullptr)
    return;
  size_t offset = 0;
  while (offset < length) {
    const size_t item_offset = offset;
    const uint8_t prefix = data[offset++];
    if (prefix == 0xFE) {
      if (length - offset < 2) {
        ESP_LOGV(TAG, "HID_ITEM offset=%u status=truncated_long_header", static_cast<unsigned>(item_offset));
        return;
      }
      const uint8_t size = data[offset++];
      const uint8_t tag = data[offset++];
      (void) item_offset;
      (void) tag;
      if (length - offset < size) {
        ESP_LOGV(TAG, "HID_ITEM offset=%u status=truncated_long_payload", static_cast<unsigned>(item_offset));
        return;
      }
      ESP_LOGV(TAG, "HID_ITEM offset=%u kind=long tag=0x%02X size=%u", static_cast<unsigned>(item_offset), tag, size);
      offset += size;
      continue;
    }
    const uint8_t size = hid_item_data_size(prefix);
    if (length - offset < size) {
      ESP_LOGV(TAG, "HID_ITEM offset=%u status=truncated_short", static_cast<unsigned>(item_offset));
      return;
    }
    ESP_LOGV(TAG, "HID_ITEM offset=%u prefix=0x%02X size=%u value=0x%08X", static_cast<unsigned>(item_offset), prefix,
             size, static_cast<unsigned>(read_unsigned_item(data + offset, size)));
    offset += size;
  }
}

bool HIDReportMap::read_bits_(const uint8_t *data, size_t length, uint32_t bit_offset, uint32_t bit_size,
                              bool signed_value, int64_t *value) {
  if (data == nullptr || value == nullptr || bit_size == 0 || bit_size > 32 || bit_offset > length * 8U ||
      bit_size > length * 8U - bit_offset)
    return false;
  uint32_t raw = 0;
  for (uint32_t bit = 0; bit < bit_size; bit++)
    if ((data[(bit_offset + bit) / 8U] & (uint8_t{1} << ((bit_offset + bit) % 8U))) != 0)
      raw |= uint32_t{1} << bit;
  if (signed_value && bit_size < 32 && (raw & (uint32_t{1} << (bit_size - 1U))) != 0)
    raw |= UINT32_MAX << bit_size;
  *value = signed_value ? static_cast<int64_t>(static_cast<int32_t>(raw)) : static_cast<int64_t>(raw);
  return true;
}

std::vector<HIDReportItemValue> HIDReportMap::parse_input_(const HIDReportSchema &report, const uint8_t *data,
                                                           size_t length) {
  std::vector<HIDReportItemValue> values;
  for (const uint16_t field_id : report.field_ids) {
    const HIDFieldSchema &field = this->fields_[field_id];
    if (field.is_constant())
      continue;
    FieldRuntime &runtime = this->runtime_[field.id];
    if (!runtime.initialized) {
      runtime.last_values.assign(field.report_count, 0);
      runtime.initialized = true;
    }

    if (field.is_variable()) {
      for (uint32_t index = 0; index < field.report_count; index++) {
        int64_t raw_value = 0;
        if (!read_bits_(data, length, field.bit_offset + index * field.report_size, field.report_size,
                        field.logical.minimum < 0, &raw_value))
          continue;
        // Out of range is no value, except for a hat switch: that is centered (as Linux reads it).
        if ((raw_value < field.logical.minimum || raw_value > field.logical.maximum) &&
            !(field.usage_at(index, true) == HIDUsage(0x39, 0x01)))
          continue;
        if ((field.is_relative() && raw_value == 0) ||
            (!field.is_relative() && runtime.last_values[index] == raw_value))
          continue;
        HIDReportItemValue value(field.usage_at(index, true), raw_value, raw_value);
        // A Main item can define several scalar fields with Report Count.
        // Their bit positions are the stable, descriptor-defined identities;
        // the Main-item ordinal alone is not unique enough for coalescing.
        value.field_id = field.bit_offset + index * field.report_size;
        value.collection_id = field.collection_id;
        value.application_usage = this->application_usage(field.collection_id);
        value.report_id = report.report_id;
        value.is_relative = field.is_relative();
        value.logical_min = static_cast<int32_t>(field.logical.minimum);
        value.logical_max = static_cast<int32_t>(field.logical.maximum);
        if (field.is_relative()) {
          value.aggregation = HIDReportItemValue::Aggregation::SUM;
        } else {
          const uint64_t span = field.logical.maximum >= field.logical.minimum
                                    ? static_cast<uint64_t>(field.logical.maximum - field.logical.minimum)
                                    : 0;
          const bool discrete_page = value.usage.page == 0x07 || value.usage.page == 0x09 || value.usage.page == 0x0C;
          if (!discrete_page && span > 16)
            value.aggregation = HIDReportItemValue::Aggregation::LATEST;
        }
        values.push_back(value);
        runtime.last_values[index] = raw_value;
      }
      continue;
    }

    std::multimap<HIDUsage, int64_t> current;
    for (uint32_t index = 0; index < field.report_count; index++) {
      int64_t raw_value = 0;
      if (!read_bits_(data, length, field.bit_offset + index * field.report_size, field.report_size,
                      field.logical.minimum < 0, &raw_value))
        continue;
      if (raw_value < field.logical.minimum || raw_value > field.logical.maximum)
        continue;
      const uint32_t usage_index = static_cast<uint32_t>(raw_value - field.logical.minimum);
      HIDUsage usage = field.usage_at(usage_index, false);
      if (usage.page != 0 && usage.usage != 0)
        current.emplace(usage, raw_value);
    }
    auto previous = runtime.active_array_usages;
    for (const auto &entry : current) {
      auto old = previous.find(entry.first);
      if (old != previous.end())
        previous.erase(old);
      else {
        HIDReportItemValue value(entry.first, 1, entry.second);
        value.field_id = field.bit_offset;
        value.collection_id = field.collection_id;
        value.application_usage = this->application_usage(field.collection_id);
        value.report_id = report.report_id;
        values.push_back(value);
      }
    }
    auto remaining = current;
    for (const auto &entry : runtime.active_array_usages) {
      auto now = remaining.find(entry.first);
      if (now != remaining.end())
        remaining.erase(now);
      else {
        HIDReportItemValue value(entry.first, 0, 0);
        value.field_id = field.bit_offset;
        value.collection_id = field.collection_id;
        value.application_usage = this->application_usage(field.collection_id);
        value.report_id = report.report_id;
        values.push_back(value);
      }
    }
    runtime.active_array_usages = std::move(current);
  }
  return values;
}

std::vector<HIDReportItemValue> HIDReportMap::parse(const HIDReportSource &source, const uint8_t *data, size_t length,
                                                    bool *recognized, HIDDecodeStatus *status) {
  if (recognized != nullptr)
    *recognized = false;
  if (status != nullptr)
    *status = HIDDecodeStatus::SCHEMA_MISSING;
  if (data == nullptr && length != 0) {
    if (status != nullptr)
      *status = HIDDecodeStatus::INVALID_PAYLOAD;
    return {};
  }
  if (source.characteristic_uuid == 0x2A22 || source.characteristic_uuid == 0x2A33) {
    if (status != nullptr)
      *status = HIDDecodeStatus::BOOT_REPORT;
    return {};
  }
  if (source.has_report_type && !is_input_kind(source.report_type)) {
    if (status != nullptr)
      *status = HIDDecodeStatus::REPORT_KIND_MISMATCH;
    return {};
  }
  const uint8_t report_id = source.has_report_id ? source.report_id : 0;
  const HIDReportSchema *report = this->find_report(HIDReportKind::INPUT, report_id);
  if (report == nullptr)
    return {};
  if (recognized != nullptr)
    *recognized = true;
  const size_t expected = report->byte_size();
  if (length < expected) {
    if (status != nullptr)
      *status = HIDDecodeStatus::SHORT;
    return {};
  }
  if (status != nullptr)
    *status = length == expected ? HIDDecodeStatus::EXACT : HIDDecodeStatus::LONG;
  auto values = this->parse_input_(*report, data, length);
  for (auto &value : values)
    value.characteristic_handle = source.characteristic_handle;
  return values;
}

}  // namespace hid
}  // namespace esphome
