#include "dynamic_lamp_output.h"

namespace esphome {
namespace dynamic_lamp {

void DynamicLamp::write_state(light::LightState *state) override {
    float bright;
    state->current_values_as_brightness(&bright);
    if (this->parent_->write_state_(this->lamp_, bright)) {
      this->state_ = state;
    }
  }

}  // namespace dynamic_lamp
}  // namespace esphome
