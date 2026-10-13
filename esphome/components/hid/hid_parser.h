#pragma once

// From fsievers22/esphome-ble-remote (MIT), pull request #45 by Gur Kohli.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace hid {

struct HIDUsage {
  uint16_t usage{0};
  uint16_t page{0};

  HIDUsage() = default;
  HIDUsage(uint16_t usage, uint16_t page) : usage(usage), page(page) {}
  bool operator==(const HIDUsage &other) const { return this->page == other.page && this->usage == other.usage; }
  bool operator<(const HIDUsage &other) const {
    return this->page < other.page || (this->page == other.page && this->usage < other.usage);
  }
};

enum class HIDReportKind : uint8_t {
  INPUT = 1,
  OUTPUT = 2,
  FEATURE = 3,
};

enum class HIDDecodeStatus : uint8_t {
  EXACT,
  LONG,
  SHORT,
  SCHEMA_MISSING,
  REPORT_KIND_MISMATCH,
  BOOT_REPORT,
  INVALID_PAYLOAD,
};

const char *hid_report_kind_name(HIDReportKind kind);
const char *hid_decode_status_name(HIDDecodeStatus status);

struct HIDReportSource {
  uint16_t service_instance{0};
  uint16_t characteristic_handle{0};
  uint8_t report_id{0};
  bool has_report_id{false};
  uint8_t report_type{0};
  bool has_report_type{false};
  bool is_notification{true};
  bool has_transport{false};
  uint16_t characteristic_uuid{0};
};

struct HIDRange {
  int64_t minimum{0};
  int64_t maximum{0};
};

struct HIDCollectionSchema {
  static constexpr uint16_t NO_PARENT = UINT16_MAX;

  uint16_t id{0};
  uint16_t parent{NO_PARENT};
  uint8_t type{0};
  HIDUsage usage{};
  std::vector<HIDUsage> aliases;
};

struct HIDFieldSchema {
  uint16_t id{0};
  HIDReportKind kind{HIDReportKind::INPUT};
  uint8_t report_id{0};
  uint16_t collection_id{HIDCollectionSchema::NO_PARENT};
  uint32_t bit_offset{0};
  uint32_t report_size{0};
  uint32_t report_count{0};
  uint16_t flags{0};
  HIDRange logical{};
  HIDRange physical{};
  int8_t unit_exponent{0};
  uint32_t unit{0};
  std::vector<HIDUsage> usages;
  std::vector<std::vector<HIDUsage>> alternative_usages;
  HIDUsage usage_minimum{};
  HIDUsage usage_maximum{};
  bool has_usage_range{false};
  std::vector<uint32_t> string_indices;
  uint32_t string_minimum{0};
  uint32_t string_maximum{0};
  bool has_string_range{false};
  std::vector<uint32_t> designator_indices;
  uint32_t designator_minimum{0};
  uint32_t designator_maximum{0};
  bool has_designator_range{false};

  bool is_constant() const { return (this->flags & 0x01U) != 0; }
  bool is_variable() const { return (this->flags & 0x02U) != 0; }
  bool is_relative() const { return (this->flags & 0x04U) != 0; }
  HIDUsage usage_at(uint32_t index, bool repeat_last) const;
};

struct HIDReportSchema {
  HIDReportKind kind{HIDReportKind::INPUT};
  uint8_t report_id{0};
  uint32_t bit_size{0};
  std::vector<uint16_t> field_ids;

  size_t byte_size() const { return (static_cast<size_t>(this->bit_size) + 7U) / 8U; }
};

struct HIDParserDiagnostic {
  size_t offset{0};
  bool error{false};
  std::string message;
};

struct HIDReportItemValue {
  enum class Aggregation : uint8_t { PASSTHROUGH, SUM, LATEST };

  HIDUsage usage{};
  HIDUsage application_usage{};
  int64_t value{0};
  int64_t raw_value{0};
  uint16_t characteristic_handle{0};
  uint32_t field_id{0};
  uint16_t collection_id{HIDCollectionSchema::NO_PARENT};
  uint8_t report_id{0};
  bool is_relative{false};
  Aggregation aggregation{Aggregation::PASSTHROUGH};

  HIDReportItemValue() = default;
  HIDReportItemValue(HIDUsage usage, int64_t value, int64_t raw_value)
      : usage(usage), value(value), raw_value(raw_value) {}
  std::string to_string() const;
};

// What a device is, from the application collections of its report descriptor.
enum HIDDeviceKind : uint8_t {
  HID_DEVICE_KEYBOARD = 1 << 0,
  HID_DEVICE_MOUSE = 1 << 1,  // mice, touchpads, pointers
  HID_DEVICE_GAMEPAD = 1 << 2,
  HID_DEVICE_REMOTE = 1 << 3,  // consumer or system controls alone
};

class HIDReportMap {
 public:
  static constexpr size_t MAX_DESCRIPTOR_BYTES = 4096;
  static constexpr size_t MAX_REPORTS = 64;
  static constexpr size_t MAX_FIELDS = 512;
  static constexpr size_t MAX_COLLECTIONS = 128;
  static constexpr size_t MAX_COLLECTION_DEPTH = 32;
  static constexpr size_t MAX_GLOBAL_STACK_DEPTH = 16;
  static constexpr uint32_t MAX_REPORT_BITS = 32768;
  static constexpr uint32_t MAX_REPORT_COUNT = 4096;

  static HIDReportMap *parse_report_map_data(const uint8_t *data, size_t length);
  static void esp_logd_report_map(const uint8_t *data, size_t length);

  std::vector<HIDReportItemValue> parse(const HIDReportSource &source, const uint8_t *data, size_t length,
                                        bool *recognized = nullptr, HIDDecodeStatus *status = nullptr);
  const HIDReportSchema *find_report(HIDReportKind kind, uint8_t report_id) const;
  const HIDFieldSchema *find_field(uint16_t field_id) const;
  HIDUsage application_usage(uint16_t collection_id) const;
  /// HIDDeviceKind bits of the device this descriptor belongs to.
  uint8_t device_kinds() const;
  void reset_runtime_state();

  const std::vector<HIDReportSchema> &reports() const { return this->reports_; }
  const std::vector<HIDFieldSchema> &fields() const { return this->fields_; }
  const std::vector<HIDCollectionSchema> &collections() const { return this->collections_; }
  const std::vector<HIDParserDiagnostic> &diagnostics() const { return this->diagnostics_; }
  bool valid() const { return this->valid_; }
  bool uses_report_ids() const { return this->uses_report_ids_; }

 private:
  struct FieldRuntime {
    std::vector<int64_t> last_values;
    std::multimap<HIDUsage, int64_t> active_array_usages;
    bool initialized{false};
  };

  std::vector<HIDReportItemValue> parse_input_(const HIDReportSchema &report, const uint8_t *data, size_t length);
  static bool read_bits_(const uint8_t *data, size_t length, uint32_t bit_offset, uint32_t bit_size, bool signed_value,
                         int64_t *value);
  void add_diagnostic_(size_t offset, bool error, std::string message);

  std::vector<HIDReportSchema> reports_;
  std::vector<HIDFieldSchema> fields_;
  std::vector<HIDCollectionSchema> collections_;
  std::vector<HIDParserDiagnostic> diagnostics_;
  std::map<uint16_t, FieldRuntime> runtime_;
  bool valid_{true};
  bool uses_report_ids_{false};
};

}  // namespace hid
}  // namespace esphome
