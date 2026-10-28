#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/network/network_component.h"
#include "esphome/components/udp/udp_component.h"
#ifdef USE_BOOT_ANNOUNCE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_BOOT_ANNOUNCE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif

namespace esphome::boot_announce {

/// After a boot that a receiver waits for (armed with watch() before the restart), sends the boot's facts
/// through the udp component until the receiver acknowledges them: started by the network component's
/// active-interface event (the network is online), resent by the scheduler only until the ack. No loop.
class BootAnnounce final : public Component
#ifdef NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT
    ,
                           public network::ActiveInterfaceListener
#endif
#ifdef USE_BOOT_ANNOUNCE_WIFI
    ,
                           public wifi::WiFiIPStateListener,
                           public wifi::WiFiConnectStateListener
#endif
#ifdef USE_BOOT_ANNOUNCE_ETHERNET
    ,
                           public ethernet::EthernetIPStateListener
#endif
{
 public:
  void set_udp(udp::UDPComponent *udp) { this->udp_ = udp; }
  void setup() override;
  void dump_config() override;
#ifdef NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT
  void on_active_interface(network::NetworkInterface interface) override;
#endif
#if defined(USE_BOOT_ANNOUNCE_WIFI) || defined(USE_BOOT_ANNOUNCE_ETHERNET)
  void on_ip_state(const network::IPAddresses &ips, const network::IPAddress &dns1,
                   const network::IPAddress &dns2) override;
#endif
#ifdef USE_BOOT_ANNOUNCE_WIFI
  void on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) override;
#endif
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
