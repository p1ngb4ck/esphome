#include "dynamic_lamp_output.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace dynamic_lamp {

static const char *const TAG = "dynamic_lamp.output";

void DynamicLamp::write_state(light::LightState *state) override {
    float bright;
    state->current_values_as_brightness(&bright);
    if (this->parent_->write_state_(this->lamp_, bright)) {
      this->state_ = state;
    }
  }

}  // namespace dynamic_lamp
}  // namespace esphome
