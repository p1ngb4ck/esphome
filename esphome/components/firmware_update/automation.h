#pragma once

#include "firmware_update.h"

#include "esphome/core/automation.h"

#include <string>

namespace esphome::firmware_update {

template<typename... Ts> class FirmwareUpdateFlashAction final : public Action<Ts...> {
 public:
  FirmwareUpdateFlashAction(FirmwareUpdateComponent *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, path)

  void play(const Ts &...x) override {
    if (this->path_.has_value()) {
      this->parent_->set_path(this->path_.value(x...));
    }
    this->parent_->flash();
    // Normally never reached due to reboot on success.
  }

 protected:
  FirmwareUpdateComponent *parent_;
};

}  // namespace esphome::firmware_update
