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
  void set_remote_desktop(remote_desktop::RemoteDesktop *desktop) { this->desktop_ = desktop; }
  void set_protocol(remote_desktop::RemoteDesktop::Protocol protocol) { this->protocol_ = protocol; }
  void set_host(const char *host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_password(const char *password) { this->password_ = password; }

  bool open() {
    if (!this->desktop_->connect(this->protocol_, this->host_, this->port_, this->password_))
      return false;
    this->session_ = this->desktop_->session();
    return true;
  }
  void close() {
    if (this->owns_session())
      this->desktop_->disconnect();
  }
  bool owns_session() const { return this->session_ != 0 && this->session_ == this->desktop_->session(); }
  bool is_open() const { return this->owns_session() && this->desktop_->is_connected(); }

 protected:
  remote_desktop::RemoteDesktop *desktop_{nullptr};
  remote_desktop::RemoteDesktop::Protocol protocol_{remote_desktop::RemoteDesktop::PROTOCOL_AUTO};
  const char *host_{""};
  uint16_t port_{5900};
  const char *password_{""};
  uint32_t session_{0};
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
