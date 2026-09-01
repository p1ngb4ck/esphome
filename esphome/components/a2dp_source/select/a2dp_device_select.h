#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <string>
#include <vector>

namespace esphome {
namespace a2dp_source {

class A2DPSource;
struct DiscoveredDevice;

/// @brief Lists the devices found in the pairing window, and pairs with the one
///        that gets picked.
///
/// The options change while a pairing window is open, which is not how a select
/// normally works -- its traits hold bare pointers and are documented as being
/// set once at startup. So the strings are owned here in a vector reserved to
/// its final size in setup(), and the traits point into that: no reallocation
/// can happen, and the pointers stay valid for as long as the program runs.
class A2DPDeviceSelect : public select::Select, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return esphome::setup_priority::DATA; }

  void set_parent(A2DPSource *parent) { this->parent_ = parent; }

  /// @brief Replaces the offered options with what the pairing window has found.
  void set_devices(const std::vector<DiscoveredDevice> &devices);

 protected:
  void control(const std::string &value) override;

  A2DPSource *parent_{nullptr};
  // Backing store for the option strings. Never resized after setup(), because
  // the traits hold pointers into it.
  std::vector<std::string> option_storage_;
};

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32
