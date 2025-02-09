#pragma once

#include "../mcp4461.h"
#include "esphome/core/component.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace mcp4461 {

class Mcp4461Wiper : public output::FloatOutput, public Parented<Mcp4461Component> {
 public:
  Mcp4461Wiper(Mcp4461Component *parent, Mcp4461WiperIdx wiper, bool terminal_a, bool terminal_b,
               bool terminal_w)
      : parent_(parent),
        wiper_(wiper),
        terminal_a_(terminal_a),
        terminal_b_(terminal_b),
        terminal_w_(terminal_w) {
    uint8_t wiper_idx = static_cast<uint8_t>(wiper);
    if (wiper_idx < 4) {
      if (parent->reg_[wiper_idx].enabled) {
        if (!terminal_a)
          parent->disable_terminal_(wiper, 'a');
        if (!terminal_b)
          parent->disable_terminal_(wiper, 'b');
        if (!terminal_w)
          parent->disable_terminal_(wiper, 'w');
      }
    }
  }
  uint16_t get_wiper_level();
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
  bool terminal_a_;
  bool terminal_b_;
  bool terminal_w_;
};

}  // namespace mcp4461
}  // namespace esphome
