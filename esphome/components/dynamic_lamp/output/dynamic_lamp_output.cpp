#include "dynamic_lamp_output.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace dynamic_lamp {

static const char *const TAG = "dynamic_lamp.output";

void DynamicLamp::write_state(float state) {
  if (this->parent_->write_state(this->lamp_, state)) {
    this->state_ = state;
}

}  // namespace dynamic_lamp
}  // namespace esphome
