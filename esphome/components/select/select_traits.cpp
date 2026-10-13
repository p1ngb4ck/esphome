#include "select_traits.h"

namespace esphome::select {

// Runtime option lists are copied, since the argument may not outlive the select; one
// out of line copy keeps a single instance of the copy code.
void SelectTraits::set_options_copy_(const char *const *options, size_t count) {
  this->options_.assign_copy(options, count);
  this->changed_(SelectOptionsChange::REPLACED, 0);
}

void SelectTraits::set_option(size_t index, const char *option) {
  if (index >= this->options_.size())
    return;
  std::vector<const char *> options(this->options_.begin(), this->options_.end());
  options[index] = option;
  this->options_.assign_copy(options.data(), options.size());
  this->changed_(SelectOptionsChange::SET, index);
}

void SelectTraits::add_option(const char *option) {
  std::vector<const char *> options(this->options_.begin(), this->options_.end());
  options.push_back(option);
  this->options_.assign_copy(options.data(), options.size());
  this->changed_(SelectOptionsChange::ADDED, options.size() - 1);
}

void SelectTraits::remove_option(size_t index) {
  if (index >= this->options_.size())
    return;
  std::vector<const char *> options(this->options_.begin(), this->options_.end());
  options.erase(options.begin() + index);
  this->options_.assign_copy(options.data(), options.size());
  this->changed_(SelectOptionsChange::REMOVED, index);
}

void SelectTraits::changed_(SelectOptionsChange change, size_t index) {
  this->revision_++;
  this->last_change_ = change;
  this->last_change_index_ = index;
}

void SelectTraits::set_options(const std::initializer_list<const char *> &options) {
  this->set_options_copy_(options.begin(), options.size());
}

void SelectTraits::set_options(const FixedVector<const char *> &options) {
  this->set_options_copy_(options.begin(), options.size());
}

}  // namespace esphome::select
