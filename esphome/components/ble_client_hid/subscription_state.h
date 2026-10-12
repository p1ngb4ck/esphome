#pragma once

#include <cstdint>

// From fsievers22/esphome-ble-remote (MIT), pull request #45 by Gur Kohli.

namespace esphome::ble_client_hid {

enum class SubscriptionDecision : uint8_t {
  UNMATCHED,
  WAIT_FOR_CCCD,
  RETRY_CCCD,
  COMPLETE,
  FAILED,
};

class SubscriptionAttemptState {
 public:
  void begin(uint16_t characteristic_handle, uint16_t cccd_handle) {
    this->characteristic_handle_ = characteristic_handle;
    this->cccd_handle_ = cccd_handle;
    this->stage_ = Stage::REGISTERING;
    this->own_cccd_write_attempted_ = false;
    this->fallback_at_ms_ = 0;
  }

  void reset() {
    this->characteristic_handle_ = 0;
    this->cccd_handle_ = 0;
    this->stage_ = Stage::IDLE;
    this->own_cccd_write_attempted_ = false;
    this->fallback_at_ms_ = 0;
  }

  SubscriptionDecision registration_result(uint16_t handle, bool success, uint32_t now_ms, uint32_t fallback_delay_ms) {
    if (this->stage_ != Stage::REGISTERING || handle != this->characteristic_handle_)
      return SubscriptionDecision::UNMATCHED;
    if (!success) {
      this->stage_ = Stage::IDLE;
      return SubscriptionDecision::FAILED;
    }
    this->stage_ = Stage::WAITING_FOR_CCCD;
    this->fallback_at_ms_ = now_ms + fallback_delay_ms;
    return SubscriptionDecision::WAIT_FOR_CCCD;
  }

  bool fallback_due(uint32_t now_ms) const {
    return this->stage_ == Stage::WAITING_FOR_CCCD && !this->own_cccd_write_attempted_ &&
           static_cast<int32_t>(now_ms - this->fallback_at_ms_) >= 0;
  }

  void own_cccd_write_attempted() { this->own_cccd_write_attempted_ = true; }

  SubscriptionDecision cccd_result(uint16_t handle, bool success) {
    if (this->stage_ != Stage::WAITING_FOR_CCCD || handle != this->cccd_handle_)
      return SubscriptionDecision::UNMATCHED;
    if (success) {
      this->stage_ = Stage::IDLE;
      return SubscriptionDecision::COMPLETE;
    }
    if (!this->own_cccd_write_attempted_)
      return SubscriptionDecision::RETRY_CCCD;
    this->stage_ = Stage::IDLE;
    return SubscriptionDecision::FAILED;
  }

 private:
  enum class Stage : uint8_t { IDLE, REGISTERING, WAITING_FOR_CCCD };

  Stage stage_{Stage::IDLE};
  uint16_t characteristic_handle_{0};
  uint16_t cccd_handle_{0};
  uint32_t fallback_at_ms_{0};
  bool own_cccd_write_attempted_{false};
};

}  // namespace esphome::ble_client_hid
