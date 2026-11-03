#pragma once

#ifdef USE_ESP32

#include "sip_phone.h"

#include "esphome/core/automation.h"

#include <string>

namespace esphome::sip_phone {

template<typename... Ts> class CallAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  TEMPLATABLE_VALUE(std::string, destination)
  void play(const Ts &...x) override { this->parent_->call(this->destination_.value(x...)); }
};

template<typename... Ts> class AnswerAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  void play(const Ts &...x) override { this->parent_->answer(); }
};

template<typename... Ts> class HangupAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  void play(const Ts &...x) override { this->parent_->hangup(); }
};

template<typename... Ts> class DeclineAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  void play(const Ts &...x) override { this->parent_->decline(); }
};

template<typename... Ts> class SendDtmfAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  TEMPLATABLE_VALUE(std::string, digits)
  void play(const Ts &...x) override { this->parent_->send_dtmf(this->digits_.value(x...)); }
};

template<typename... Ts> class SendMessageAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  TEMPLATABLE_VALUE(std::string, destination)
  TEMPLATABLE_VALUE(std::string, text)
  void play(const Ts &...x) override {
    this->parent_->send_message(this->destination_.value(x...), this->text_.value(x...));
  }
};

template<typename... Ts> class SetMuteAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  TEMPLATABLE_VALUE(bool, value)
  void play(const Ts &...x) override { this->parent_->set_mute(this->value_.value(x...)); }
};

template<typename... Ts> class SetDndAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  TEMPLATABLE_VALUE(bool, value)
  void play(const Ts &...x) override { this->parent_->set_dnd(this->value_.value(x...)); }
};

template<typename... Ts> class SetAutoAnswerAction final : public Action<Ts...>, public Parented<SipPhone> {
 public:
  TEMPLATABLE_VALUE(bool, value)
  void play(const Ts &...x) override { this->parent_->set_auto_answer(this->value_.value(x...)); }
};

}

#endif
