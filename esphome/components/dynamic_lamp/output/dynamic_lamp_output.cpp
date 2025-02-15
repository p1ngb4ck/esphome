#include "dynamic_lamp_output.h"

namespace esphome {
namespace dynamic_lamp {

void DynamicLamp::write_state(float state) {
  if (this->parent_->write_state_(this->lamp_, state))
  {
    this->state_ = state;
  }
}

}  // namespace dynamic_lamp
}  // namespace esphome
