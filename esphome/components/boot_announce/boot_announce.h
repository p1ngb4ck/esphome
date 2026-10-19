#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/udp/udp_component.h"

namespace esphome::boot_announce {

/// After a boot that a receiver waits for (armed with watch() before the restart), sends the boot's facts
/// through the udp component until the receiver acknowledges them. Unarmed it stays idle (no loop).
class BootAnnounce final : public Component {
 public:
  void set_udp(udp::UDPComponent *udp) { this->udp_ = udp; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the udp component (AFTER_WIFI): its send socket exists only once its setup() ran, and a loop() of
  // ours before that would send through a null socket.
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  /// A receiver waits for the next boot: the mark in RTC memory (kept over a restart, a crash, an OTA reboot).
  void watch();

 protected:
  void send_();
  udp::UDPComponent *udp_{nullptr};
  bool acked_{false};
  uint32_t last_send_{0};
};

template<typename... Ts> class WatchAction final : public Action<Ts...>, public Parented<BootAnnounce> {
 public:
  void play(const Ts &...x) override { this->parent_->watch(); }
};

}  // namespace esphome::boot_announce
