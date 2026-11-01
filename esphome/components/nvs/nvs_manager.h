#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ESP32_NVS_GUARD)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include <string>

namespace esphome::nvs {

class NvsManager : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

#ifdef USE_TEXT_SENSOR
  void set_state_sensor(text_sensor::TextSensor *sensor) { this->state_ = sensor; }
#endif
  Trigger<std::string, std::string> *get_check_trigger() { return &this->check_trigger_; }

  bool check();
  bool invalidate(const std::string &ns, const std::string &key);
  bool reset(bool keep_protected, bool reboot);
  bool request(const std::string &policy);
#ifdef USE_STORAGE
  bool backup(const std::string &path);
  bool restore(const std::string &path, bool protected_only, bool reboot);
#endif

 protected:
  void after_setup_();
  void publish_(size_t entries, uint16_t unreadable);

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *state_{nullptr};
#endif
  Trigger<std::string, std::string> check_trigger_;
};

template<typename... Ts> class CheckAction : public Action<Ts...>, public Parented<NvsManager> {
 public:
  void play(const Ts &...x) override { this->parent_->check(); }
};

template<typename... Ts> class InvalidateAction : public Action<Ts...>, public Parented<NvsManager> {
 public:
  TEMPLATABLE_VALUE(std::string, ns)
  TEMPLATABLE_VALUE(std::string, key)
  void play(const Ts &...x) override { this->parent_->invalidate(this->ns_.value(x...), this->key_.value(x...)); }
};

template<typename... Ts> class ResetAction : public Action<Ts...>, public Parented<NvsManager> {
 public:
  void set_keep_protected(bool keep) { this->keep_protected_ = keep; }
  void set_reboot(bool reboot) { this->reboot_ = reboot; }
  void play(const Ts &...x) override { this->parent_->reset(this->keep_protected_, this->reboot_); }

 protected:
  bool keep_protected_{true};
  bool reboot_{true};
};

template<typename... Ts> class RequestAction : public Action<Ts...>, public Parented<NvsManager> {
 public:
  TEMPLATABLE_VALUE(std::string, policy)
  void play(const Ts &...x) override { this->parent_->request(this->policy_.value(x...)); }
};

#ifdef USE_STORAGE
template<typename... Ts> class BackupAction : public Action<Ts...>, public Parented<NvsManager> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override { this->parent_->backup(this->path_.value(x...)); }
};

template<typename... Ts> class RestoreAction : public Action<Ts...>, public Parented<NvsManager> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void set_protected_only(bool only) { this->protected_only_ = only; }
  void set_reboot(bool reboot) { this->reboot_ = reboot; }
  void play(const Ts &...x) override {
    this->parent_->restore(this->path_.value(x...), this->protected_only_, this->reboot_);
  }

 protected:
  bool protected_only_{false};
  bool reboot_{true};
};
#endif

}

#endif
