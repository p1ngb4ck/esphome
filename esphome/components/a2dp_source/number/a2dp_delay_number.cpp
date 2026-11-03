#include "a2dp_delay_number.h"

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#include "../a2dp_source.h"

namespace esphome {
namespace a2dp_source {

void A2DPDelayNumber::control(float value) {
  this->parent_->set_output_delay_ms(value > 0 ? static_cast<uint32_t>(value + 0.5f) : 0);
}

}  // namespace a2dp_source
}  // namespace esphome

#endif
