#pragma once

#include "ps_tools.h"
#include "esphome/core/automation.h"

namespace esphome::ps_tools {

template<typename... Ts> class StartGlitchAction : public Action<Ts...> {
 public:
  explicit StartGlitchAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_glitch(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class StartGlitchWriteAction : public Action<Ts...> {
 public:
  explicit StartGlitchWriteAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_glitch_write(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->stop(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class StartWriteAction : public Action<Ts...> {
 public:
  explicit StartWriteAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_write(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class StartReadAction : public Action<Ts...> {
 public:
  explicit StartReadAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_read(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class StartReadAction : public Action<Ts...> {
 public:
  explicit StartOCDReadAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_ocd_read(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class StartBlankCheckAction : public Action<Ts...> {
 public:
  explicit StartBlankCheckAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_blank_check(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class AnalyzeNorAction : public Action<Ts...> {
 public:
  explicit AnalyzeNorAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_analyze_nor(); }

 protected:
  PsTools *parent_;
};

template<typename... Ts> class ProbeSysconAction : public Action<Ts...> {
 public:
  explicit ProbeSysconAction(PsTools *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_probe_syscon(); }

 protected:
  PsTools *parent_;
};

}  // namespace esphome::ps_tools
