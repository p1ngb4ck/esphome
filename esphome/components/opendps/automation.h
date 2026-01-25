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

// Start Calibration Assistant Action
template<typename... Ts> class StartCalibrationAssistantAction : public Action<Ts...> {
 public:
  explicit StartCalibrationAssistantAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(float, vin_low_mv)
  TEMPLATABLE_VALUE(float, vin_high_mv)
  TEMPLATABLE_VALUE(float, vout_low_mv)
  TEMPLATABLE_VALUE(float, vout_high_mv)
  TEMPLATABLE_VALUE(float, load_resistance)
  TEMPLATABLE_VALUE(float, load_max_wattage)
  TEMPLATABLE_VALUE(float, max_dps_current)

  void play(Ts... x) override {
    CalibrationAssistantParams params;
    params.vin_low_mv = this->vin_low_mv_.value(x...);
    params.vin_high_mv = this->vin_high_mv_.value(x...);
    params.vout_low_mv = this->vout_low_mv_.value(x...);
    params.vout_high_mv = this->vout_high_mv_.value(x...);
    params.load_resistance = this->load_resistance_.value(x...);
    params.load_max_wattage = this->load_max_wattage_.value(x...);
    params.max_dps_current = this->max_dps_current_.value(x...);
    this->parent_->start_calibration_assistant(params);
  }

 protected:
  OpenDPS *parent_;
};

// Calibration Assistant Step Action
template<typename... Ts> class CalibrationAssistantStepAction : public Action<Ts...> {
 public:
  explicit CalibrationAssistantStepAction(OpenDPS *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(float, measured_value)

  void play(Ts... x) override {
    float value = this->measured_value_.value(x...);
    this->parent_->calibration_assistant_step(value);
  }

 protected:
  OpenDPS *parent_;
};

// Cancel Calibration Assistant Action
template<typename... Ts> class CancelCalibrationAssistantAction : public Action<Ts...> {
 public:
  explicit CancelCalibrationAssistantAction(OpenDPS *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->cancel_calibration_assistant(); }

 protected:
  OpenDPS *parent_;
};

}  // namespace opendps
}  // namespace esphome
