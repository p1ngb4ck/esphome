#include "mcp4461_output.h"
#include <cmath>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp4461 {

static const char *const TAG = "mcp4461.output";

void Mcp4461Wiper::write_state(float state) {
  if (this->parent_->set_wiper_level_(this->wiper_, static_cast<uint16_t>(state * 1000))) {
    this->state_ = state;
  }
}

uint16_t Mcp4461Wiper::read_state() { return this->parent_->get_wiper_level_(this->wiper_); }

uint16_t Mcp4461Wiper::update_state() {
  this->state_ = this->read_state();
  return this->state_;
}

void Mcp4461Wiper::enable_wiper() {
  this->parent_->enable_wiper_(static_cast<uint8_t>(this->wiper_));
}

void Mcp4461Wiper::disable_wiper() {
  this->parent_->disable_wiper_(static_cast<uint8_t>(this->wiper_));
}

void Mcp4461Wiper::increase_wiper() {
  if (this->parent_->increase_wiper_(this->wiper_)) {
    this->state_ = this->state_ + 1.0;
  }
}

void Mcp4461Wiper::decrease_wiper() {
  if (this->parent_->decrease_wiper_(this->wiper_)) {
    this->state_ = this->state_ - 1.0;
  }
}

void Mcp4461Wiper::enable_terminal(char terminal) {
  this->parent_->enable_terminal_(this->wiper_, terminal);
}

void Mcp4461Wiper::disable_terminal(char terminal) {
  this->parent_->disable_terminal_(this->wiper_, terminal);
}

}  // namespace mcp4461
}  // namespace esphome
