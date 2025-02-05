#pragma once

#include "../mcp4461.h"
#include "esphome/core/component.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace mcp4461 {

class Mcp4461Wiper : public output::FloatOutput {
 public:
  Mcp4461Wiper(Mcp4461Component *parent, Mcp4461WiperIdx wiper, bool enable, bool terminal_a, bool terminal_b,
               bool terminal_w, float initial_value = 1.0)
      : parent_(parent),
        wiper_(wiper),
        enable_(enable),
        terminal_a_(terminal_a),
        terminal_b_(terminal_b),
        terminal_w_(terminal_w) {
    uint8_t wiper_idx = static_cast<uint8_t>(wiper);
    // update wiper connection state
    if (!(this->enable_) && wiper_idx < 4) {
      parent->reg_[wiper_idx].enabled = false;
      parent->disable_terminal(wiper, 'h');
    }
    if (!terminal_a && wiper_idx < 4)
      parent->disable_terminal(wiper, 'a');
    if (!terminal_b && wiper_idx < 4)
      parent->disable_terminal(wiper, 'b');
    if (!terminal_w && wiper_idx < 4)
      parent->disable_terminal(wiper, 'w');
  }
  uint16_t get_wiper_level();
  void save_level();
  void enable_wiper();
  void disable_wiper();
  void increase_wiper();
  void decrease_wiper();
  void enable_terminal(char terminal);
  void disable_terminal(char terminal);
  void set_initial_value(float initial_value) { this->initial_value_ = initial_value; }

 protected:
  void write_state(float state) override;
  Mcp4461Component *parent_;
  Mcp4461WiperIdx wiper_;
  bool enable_;
  uint16_t state_;
  optional<float> initial_value_;
  bool terminal_a_;
  bool terminal_b_;
  bool terminal_w_;
};

}  // namespace mcp4461
}  // namespace esphome
