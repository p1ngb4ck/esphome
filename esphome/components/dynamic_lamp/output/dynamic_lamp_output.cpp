#include "dynamic_lamp_output.h"

namespace esphome {
namespace dynamic_lamp {

void DynamicLamp::write_state(float state) {
  if (this->active_lamps_[this->lamp_].set_level(state))
  {
    this->state_ = state;
  }
}

}  // namespace dynamic_lamp
}  // namespace esphome
