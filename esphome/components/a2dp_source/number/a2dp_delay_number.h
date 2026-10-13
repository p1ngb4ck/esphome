#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#include "esphome/components/number/number.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace a2dp_source {

class A2DPSource;

/// @brief The connected device's output delay; set here, kept per device.
class A2DPDelayNumber : public number::Number, public Parented<A2DPSource> {
 protected:
  void control(float value) override;
};

}  // namespace a2dp_source
}  // namespace esphome

#endif
