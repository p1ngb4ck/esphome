#pragma once

#include "../mcp4461.h"
#include "esphome/core/component.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace mcp4461 {

class Mcp4461Wiper : public output::FloatOutput, public Parented<Mcp4461Component> {
 public:
  Mcp4461Wiper(Mcp4461Component *parent, Mcp4461WiperIdx wiper) : parent_(parent), wiper_(wiper) {}
  }
  uint16_t read_state();
  uint16_t update_state();
  void save_level();
  void enable_wiper();
  void disable_wiper();
  void increase_wiper();
  void decrease_wiper();
  void enable_terminal(char terminal);
  void disable_terminal(char terminal);

 protected:
  void write_state(float state) override;
  Mcp4461Component *parent_;
  Mcp4461WiperIdx wiper_;
  uint16_t state_;
  optional<uint16_t> initial_value_;
};

}  // namespace mcp4461
}  // namespace esphome
