#pragma once

#include "../dynamic_lamp.h"
#include "esphome/core/component.h"
#include "esphome/components/output/float_output.h"

namespace esphome {
namespace dnamic_lamp {

class DynamicLamp : public output::FloatOutput, public Parented<DynamicLampComponent> {
 public:
 DynamicLamp(DynamicLampComponent *parent, DynamicLampIdx lamp) : parent_(parent), lamp_(lamp) {}

 protected:
  void write_state(float state) override;
  DynamicLampComponent *parent_;
  DynamicLampIdx lamp_;
  float state_;
};

}  // namespace dynamic_lamp
}  // namespace esphome
