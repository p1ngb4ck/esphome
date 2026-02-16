#pragma once

#include "sysglitch.h"
#include "esphome/core/automation.h"

namespace esphome::sysglitch {

template<typename... Ts> class StartGlitchAction : public Action<Ts...> {
 public:
  explicit StartGlitchAction(SysGlitch *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->start_glitch(); }

 protected:
  SysGlitch *parent_;
};

template<typename... Ts> class StopGlitchAction : public Action<Ts...> {
 public:
  explicit StopGlitchAction(SysGlitch *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->stop_glitch(); }

 protected:
  SysGlitch *parent_;
};

template<typename... Ts> class StartWriteAction : public Action<Ts...> {
 public:
  explicit StartWriteAction(SysGlitch *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->start_write(); }

 protected:
  SysGlitch *parent_;
};

}  // namespace esphome::sysglitch
