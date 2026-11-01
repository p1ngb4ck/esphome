#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

#include <cstddef>

namespace esphome::apps {

class AppSlot {
 public:
  void set_excluded(AppSlot *const *apps, size_t count) {
    this->excluded_ = apps;
    this->excluded_count_ = count;
  }
  bool is_running() const { return this->running_; }
  void start();
  void stop();
  Trigger<> *get_start_trigger() { return &this->start_trigger_; }
  Trigger<> *get_stop_trigger() { return &this->stop_trigger_; }

 protected:
  AppSlot *const *excluded_{nullptr};
  size_t excluded_count_{0};
  bool running_{false};
  Trigger<> start_trigger_;
  Trigger<> stop_trigger_;
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<AppSlot> {
 public:
  void play(const Ts &...x) override { this->parent_->start(); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<AppSlot> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<AppSlot> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
};

}
