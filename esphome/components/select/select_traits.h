#pragma once

#include "esphome/core/helpers.h"
#include <initializer_list>
#include <vector>

namespace esphome::select {

/// Option strings: a shared codegen table, or a copy of a runtime list.
using SelectOptions = ConstVector<const char *, true>;

/// The change that produced the current options revision.
enum class SelectOptionsChange : uint8_t { REPLACED, SET, ADDED, REMOVED };

class SelectTraits {
 public:
  SelectTraits() = default;
  SelectTraits(const SelectTraits &) = delete;
  SelectTraits &operator=(const SelectTraits &) = delete;

  /// Codegen only: points at a table that outlives the select. Call before any runtime set_options;
  /// it does not free a previous copy (generated setup() runs before any lambda or automation).
  void set_options_static(const char *const *options, size_t count) { this->options_.assign_static(options, count); }
  /// Runtime lists: the pointer list is copied, as before; the strings must still outlive the select.
  void set_options(const SelectOptions &options) { this->set_options_copy_(options.data(), options.size()); }
  void set_options(const std::initializer_list<const char *> &options);
  void set_options(const FixedVector<const char *> &options);
  /// Single slots of a runtime list; the string must outlive the select, as with set_options.
  void set_option(size_t index, const char *option);
  void add_option(const char *option);
  void remove_option(size_t index);
  const SelectOptions &get_options() const { return this->options_; }

  /// Counts every change to the options.
  uint32_t get_options_revision() const { return this->revision_; }
  SelectOptionsChange get_last_change() const { return this->last_change_; }
  /// The slot the last SET, ADDED or REMOVED change touched.
  size_t get_last_change_index() const { return this->last_change_index_; }

 protected:
  void set_options_copy_(const char *const *options, size_t count);
  void changed_(SelectOptionsChange change, size_t index);

  SelectOptions options_;
  uint32_t revision_{0};
  uint16_t last_change_index_{0};
  SelectOptionsChange last_change_{SelectOptionsChange::REPLACED};
};

}  // namespace esphome::select
