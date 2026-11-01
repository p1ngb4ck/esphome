#pragma once

#include "esphome/core/defines.h"
#ifdef USE_BROWSER

#include "esphome/components/remote_desktop/remote_desktop.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <new>

namespace esphome::browser {

class Browser : public Component {
 public:
  static Browser *create() {
    void *mem = RAMAllocator<Browser>(RAMAllocator<Browser>::ALLOC_EXTERNAL).allocate(1);
    return mem != nullptr ? new (mem) Browser() : new Browser();
  }
  void setup() override {
    this->desktop_->add_on_change_callback([this]() {
      if (!this->desktop_->is_connected())
        this->opened_ = false;
    });
  }
  void set_remote_desktop(remote_desktop::RemoteDesktop *desktop) { this->desktop_ = desktop; }
  void set_protocol(remote_desktop::RemoteDesktop::Protocol protocol) { this->protocol_ = protocol; }
  void set_host(const char *host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_password(const char *password) { this->password_ = password; }

  bool open() {
    this->opened_ = this->desktop_->connect(this->protocol_, this->host_, this->port_, this->password_);
    return this->opened_;
  }
  void close() {
    if (this->opened_)
      this->desktop_->disconnect();
  }
  bool is_open() const { return this->opened_ && this->desktop_->is_connected(); }

 protected:
  remote_desktop::RemoteDesktop *desktop_{nullptr};
  remote_desktop::RemoteDesktop::Protocol protocol_{remote_desktop::RemoteDesktop::PROTOCOL_AUTO};
  const char *host_{""};
  uint16_t port_{5900};
  const char *password_{""};
  bool opened_{false};
};

template<typename... Ts> class OpenAction : public Action<Ts...>, public Parented<Browser> {
 public:
  void play(const Ts &...x) override { this->parent_->open(); }
};

template<typename... Ts> class CloseAction : public Action<Ts...>, public Parented<Browser> {
 public:
  void play(const Ts &...x) override { this->parent_->close(); }
};

template<typename... Ts> class IsOpenCondition : public Condition<Ts...>, public Parented<Browser> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_open(); }
};

}

#endif
