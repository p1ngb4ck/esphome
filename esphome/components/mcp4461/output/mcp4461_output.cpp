#include "mcp4461_output.h"
#include <cmath>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp4461 {

static const char *const TAG = "mcp4461.output";

void Mcp4461Wiper::write_state(float state) {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  ESP_LOGV(TAG, "Got value %02f from frontend", state);
  const float max_taps = 256.0;
  state = state * 1000.0;
  if (state > max_taps) {
    ESP_LOGW(TAG, "Cannot set taps > 0.256 for wiper %" PRIu8 ", clamping to 0.256 !", wiper_idx);
    state = 256.0;
  }
  uint16_t taps;
  taps = static_cast<uint16_t>(state);
  ESP_LOGV(TAG, "Setting wiper %" PRIu8 " to value %" PRIu16 "", wiper_idx, taps);
  this->state_ = state;
  this->parent_->set_wiper_level(this->wiper_, taps);
}

uint16_t Mcp4461Wiper::get_wiper_level() { return this->parent_->get_wiper_level(this->wiper_); }

void Mcp4461Wiper::save_level() {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot save level for nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  Mcp4461WiperIdx nonvolatile_wiper = wiper_idx + 4;
  this->parent_->set_wiper_level(nonvolatile_wiper, this->state_);
}

void Mcp4461Wiper::enable_wiper() {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot enable nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->enable_wiper(this->wiper_);
}

void Mcp4461Wiper::disable_wiper() {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot disable nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->disable_wiper(this->wiper_);
}

void Mcp4461Wiper::increase_wiper() {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot increase nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->increase_wiper(this->wiper_);
}

void Mcp4461Wiper::decrease_wiper() {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot decrease nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->decrease_wiper(this->wiper_);
}

void Mcp4461Wiper::enable_terminal(char terminal) {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot get/set terminals nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->enable_terminal(this->wiper_, terminal);
}

void Mcp4461Wiper::disable_terminal(char terminal) {
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot get/set terminals for nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->disable_terminal(this->wiper_, terminal);
}

}  // namespace mcp4461
}  // namespace esphome
