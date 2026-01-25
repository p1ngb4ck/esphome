#pragma once

#include "esphome/core/automation.h"
#include "opendps.h"

namespace esphome {
namespace opendps {

// Enable Output Action
template<typename... Ts> class EnableOutputAction : public Action<Ts...> {
 public:
  explicit EnableOutputAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(bool, enable)

  void play(Ts... x) override {
    bool enable = this->enable_.value(x...);
    this->parent_->enable_output(enable);
  }

 protected:
  OpenDPS *parent_;
};

// Set Voltage Action
template<typename... Ts> class SetVoltageAction : public Action<Ts...> {
 public:
  explicit SetVoltageAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(float, voltage)

  void play(Ts... x) override {
    float voltage = this->voltage_.value(x...);
    this->parent_->set_voltage(voltage);
  }

 protected:
  OpenDPS *parent_;
};

// Set Current Action
template<typename... Ts> class SetCurrentAction : public Action<Ts...> {
 public:
  explicit SetCurrentAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(float, current)

  void play(Ts... x) override {
    float current = this->current_.value(x...);
    this->parent_->set_current(current);
  }

 protected:
  OpenDPS *parent_;
};

// Set Function Action
template<typename... Ts> class SetFunctionAction : public Action<Ts...> {
 public:
  explicit SetFunctionAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, function)

  void play(Ts... x) override {
    std::string function = this->function_.value(x...);
    this->parent_->set_function(function);
  }

 protected:
  OpenDPS *parent_;
};

// Set Parameter Action
template<typename... Ts> class SetParameterAction : public Action<Ts...> {
 public:
  explicit SetParameterAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, key)
  TEMPLATABLE_VALUE(std::string, value)

  void play(Ts... x) override {
    std::string key = this->key_.value(x...);
    std::string value = this->value_.value(x...);
    this->parent_->set_parameter(key, value);
  }

 protected:
  OpenDPS *parent_;
};

// Lock Action
template<typename... Ts> class LockAction : public Action<Ts...> {
 public:
  explicit LockAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(bool, locked)

  void play(Ts... x) override {
    bool locked = this->locked_.value(x...);
    this->parent_->lock(locked);
  }

 protected:
  OpenDPS *parent_;
};

// Set Brightness Action
template<typename... Ts> class SetBrightnessAction : public Action<Ts...> {
 public:
  explicit SetBrightnessAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(uint8_t, brightness)

  void play(Ts... x) override {
    uint8_t brightness = this->brightness_.value(x...);
    this->parent_->set_brightness(brightness);
  }

 protected:
  OpenDPS *parent_;
};

// Ping Action
template<typename... Ts> class PingAction : public Action<Ts...> {
 public:
  explicit PingAction(OpenDPS *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->send_ping(); }

 protected:
  OpenDPS *parent_;
};

// Request Version Action
template<typename... Ts> class RequestVersionAction : public Action<Ts...> {
 public:
  explicit RequestVersionAction(OpenDPS *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->request_version(); }

 protected:
  OpenDPS *parent_;
};

// Upgrade Firmware Action
template<typename... Ts> class UpgradeFirmwareAction : public Action<Ts...> {
 public:
  explicit UpgradeFirmwareAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, firmware_path)

  void play(Ts... x) override {
    std::string path = this->firmware_path_.value(x...);
    this->parent_->start_firmware_upgrade(path);
  }

 protected:
  OpenDPS *parent_;
};

// Request Calibration Report Action
template<typename... Ts> class RequestCalibrationReportAction : public Action<Ts...> {
 public:
  explicit RequestCalibrationReportAction(OpenDPS *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->request_calibration_report(); }

 protected:
  OpenDPS *parent_;
};

// Set Calibration Action
template<typename... Ts> class SetCalibrationAction : public Action<Ts...> {
 public:
  explicit SetCalibrationAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, name)
  TEMPLATABLE_VALUE(float, value)

  void play(Ts... x) override {
    std::string name = this->name_.value(x...);
    float value = this->value_.value(x...);
    this->parent_->set_calibration(name, value);
  }

 protected:
  OpenDPS *parent_;
};

// Clear Calibration Action
template<typename... Ts> class ClearCalibrationAction : public Action<Ts...> {
 public:
  explicit ClearCalibrationAction(OpenDPS *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->clear_calibration(); }

 protected:
  OpenDPS *parent_;
};

}  // namespace opendps
}  // namespace esphome
