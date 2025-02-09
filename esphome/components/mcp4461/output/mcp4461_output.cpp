#include "mcp4461_output.h"
#include <cmath>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp4461 {

static const char *const TAG = "mcp4461.output";
static const LogString *get_wiper_message_string(int status) {
  switch (status) {
    case Mcp4461Component::MCP4461_FAILED:
      return LOG_STR("Parent MCP4461 component failed");
    default:
      return LOG_STR("Unknown error");
  }
}

void Mcp4461Wiper::write_state(float state) {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
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
  this->parent_->set_wiper_level_(this->wiper_, taps);
}

uint16_t Mcp4461Wiper::read_state() { return this->parent_->get_wiper_level_(this->wiper_); }

uint16_t Mcp4461Wiper::update_state() {
  this->state_ = this->read_state(this->wiper_);
  return this->state_;
}

void Mcp4461Wiper::save_level() {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot save level for nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  uint8_t nonvolatile_wiper_idx = wiper_idx + 4;
  this->parent_->reg_[nonvolatile_wiper_idx].state = this->parent_->reg_[wiper_idx].state;
  Mcp4461WiperIdx nonvolatile_wiper = static_cast<Mcp4461WiperIdx>(nonvolatile_wiper_idx);
  this->parent_->set_wiper_level_(nonvolatile_wiper, this->state_);
}

void Mcp4461Wiper::enable_wiper() {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  this->parent_->enable_wiper_(this->wiper_);
}

void Mcp4461Wiper::disable_wiper() {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  this->parent_->disable_wiper_(this->wiper_);
}

void Mcp4461Wiper::increase_wiper() {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot increase nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  if (this->parent_->increase_wiper_(this->wiper_)) {
    this->state_ = this->state_ + 1.0;
  }
}

void Mcp4461Wiper::decrease_wiper() {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot decrease nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  if (this->parent_->decrease_wiper_(this->wiper_)) {
    this->state_ = this->state_ - 1.0;
  }
}

void Mcp4461Wiper::enable_terminal(char terminal) {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot get/set terminals nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->enable_terminal_(this->wiper_, terminal);
}

void Mcp4461Wiper::disable_terminal(char terminal) {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "%s", LOG_STR_ARG(get_wiper_message_string(Mcp4461Component::MCP4461_FAILED)));
    return;
  }
  uint8_t wiper_idx = static_cast<uint8_t>(this->wiper_);
  if (wiper_idx > 3) {
    ESP_LOGW(TAG, "Cannot get/set terminals for nonvolatile wiper %" PRIu8 " !", wiper_idx);
    return;
  }
  this->parent_->disable_terminal_(this->wiper_, terminal);
}

}  // namespace mcp4461
}  // namespace esphome
