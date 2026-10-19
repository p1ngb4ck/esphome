#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/network/network_component.h"
#include "esphome/components/udp/udp_component.h"

namespace esphome::boot_announce {

/// After a boot that a receiver waits for (armed with watch() before the restart), sends the boot's facts
/// through the udp component until the receiver acknowledges them: started by the network component's
/// active-interface event (the network is online), resent by the scheduler only until the ack. No loop.
class BootAnnounce final : public Component, public network::ActiveInterfaceListener {
 public:
  void set_udp(udp::UDPComponent *udp) { this->udp_ = udp; }
  void setup() override;
  void dump_config() override;
  void on_active_interface(network::NetworkInterface interface) override;
  // After the udp component (AFTER_WIFI): setup() may already send (network online) through its socket.
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  /// A receiver waits for the next boot: the mark in RTC memory (kept over a restart, a crash, an OTA reboot).
  void watch();

 protected:
  void send_();
  void start_();
  void stop_();
  udp::UDPComponent *udp_{nullptr};
  bool waiting_{false};  // armed for this boot and not acknowledged yet
};

template<typename... Ts> class WatchAction final : public Action<Ts...>, public Parented<BootAnnounce> {
 public:
  void play(const Ts &...x) override { this->parent_->watch(); }
};

}  // namespace esphome::boot_announce
