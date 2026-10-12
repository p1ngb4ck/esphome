#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/hid/hid_parser.h"

// From fsievers22/esphome-ble-remote (MIT), pull request #45 by Gur Kohli.

namespace esphome::ble_client_hid {

using hid::HIDReportItemValue;

struct HIDEvent {
  std::map<std::string, std::string> data;
};

struct SampledEventKey {
  uint16_t service_instance;
  uint32_t field_id;
  uint8_t report_id;
  uint8_t report_kind;

  bool operator==(const SampledEventKey &other) const {
    return this->service_instance == other.service_instance && this->field_id == other.field_id &&
           this->report_id == other.report_id && this->report_kind == other.report_kind;
  }
};

// A deliberately small, insertion-ordered buffer. HID reports normally expose
// only a handful of continuous usages, so a linear search avoids the ordering
// loss and allocation overhead of a map plus a separate ordering structure.
class OrderedEventBuffer {
 public:
  explicit OrderedEventBuffer(size_t capacity = 16) : capacity_(capacity) {}

  bool accumulate(const SampledEventKey &key, HIDReportItemValue::Aggregation aggregation, int64_t value,
                  int64_t raw_value, HIDEvent event) {
    for (auto &pending : this->pending_) {
      if (!(pending.key == key))
        continue;
      if (aggregation == HIDReportItemValue::Aggregation::SUM) {
        pending.value = saturating_add_(pending.value, value);
        pending.raw_value = saturating_add_(pending.raw_value, raw_value);
      } else {
        pending.value = value;
        pending.raw_value = raw_value;
      }
      const std::string seq_id_from = pending.event.data.at("seq_id_from");
      pending.event = std::move(event);
      pending.event.data["value"] = std::to_string(pending.value);
      pending.event.data["raw_value"] = std::to_string(pending.raw_value);
      pending.event.data["seq_id_from"] = seq_id_from;
      return true;
    }
    if (this->pending_.size() >= this->capacity_)
      return false;
    event.data["value"] = std::to_string(value);
    event.data["raw_value"] = std::to_string(raw_value);
    this->pending_.push_back({key, aggregation, value, raw_value, std::move(event)});
    return true;
  }

  std::vector<HIDEvent> drain() {
    std::vector<HIDEvent> events;
    events.reserve(this->pending_.size());
    for (auto &pending : this->pending_)
      events.push_back(std::move(pending.event));
    this->pending_.clear();
    return events;
  }

  std::vector<HIDEvent> drain_before(HIDEvent barrier) {
    std::vector<HIDEvent> events = this->drain();
    events.push_back(std::move(barrier));
    return events;
  }

  void clear() { this->pending_.clear(); }
  bool empty() const { return this->pending_.empty(); }
  size_t size() const { return this->pending_.size(); }

 private:
  struct PendingEvent {
    SampledEventKey key;
    HIDReportItemValue::Aggregation aggregation;
    int64_t value;
    int64_t raw_value;
    HIDEvent event;
  };

  static int64_t saturating_add_(int64_t left, int64_t right) {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
      return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
      return std::numeric_limits<int64_t>::min();
    return left + right;
  }

  const size_t capacity_;
  std::vector<PendingEvent> pending_;
};

}  // namespace esphome::ble_client_hid
