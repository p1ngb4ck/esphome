#pragma once

#include "../dynamic_lamp.h"

namespace esphome {
namespace dynamic_monochromatic_light {

class DynamicLamp : public light::LightOutput, public Parented<DynamicLampComponent> {
 public:
  DynamicLamp(DynamicLampComponent *parent, DynamicLampIdx lamp) : parent_(parent), lamp_(lamp) {}
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }

 protected:
  void write_state(light::LightState *state) override;
  DynamicLampComponent *parent_;
  DynamicLampIdx lamp_;
  float state_;
};

}  // namespace dynamic_monochromatic_light
}  // namespace esphome
