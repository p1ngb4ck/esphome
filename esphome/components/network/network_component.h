#pragma once
#include "esphome/core/defines.h"
#if defined(USE_NETWORK) && defined(USE_ESP32)
#include "esphome/core/component.h"

#ifdef USE_NETWORK_DEFAULT_ROUTE
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
// Forward declaration matching esp_netif's own typedef; avoids pulling esp_netif.h
// into this header.
using esp_netif_t = struct esp_netif_obj;
#endif

namespace esphome::network {

#ifdef USE_NETWORK_DEFAULT_ROUTE
/// Call right after `changed` got a static address or (re)started its DHCP client: ESP-IDF then
/// clears lwIP's global DNS servers, and when `changed` is not the default interface nothing
/// writes them again. Puts the default interface's servers back at once.
void restore_default_dns(esp_netif_t *changed);

/// The interface holding the default route. Lists only the interfaces loop() arbitrates
/// (ARBITRATED_NETWORK_TYPES in __init__.py); extend both together.
enum class NetworkInterface : uint8_t {
  NETWORK_INTERFACE_NONE,
  NETWORK_INTERFACE_ETHERNET,
  NETWORK_INTERFACE_WIFI,
  NETWORK_INTERFACE_USB_ETHERNET,
};

#ifdef NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT
/// Notified from the main loop when the interface holding the default route changes.
class ActiveInterfaceListener {
 public:
  virtual void on_active_interface(NetworkInterface interface) = 0;
};
#endif
#endif

class NetworkComponent final : public Component {
 public:
  NetworkComponent();
  void setup() override;
  void dump_config() override;
  // AFTER_BLUETOOTH: BLE controller must initialize before esp_netif_init per IDF guidance.
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

#ifdef USE_NETWORK_DEFAULT_ROUTE
  void loop() override;

  /// The first connected interface in the priority order; NONE while none is connected.
  NetworkInterface get_active_interface() const { return this->active_interface_; }

  /// The priority order: network: priority: until it is changed at runtime, then the saved one.
  void add_interface(NetworkInterface interface) { this->order_.push_back(interface); }
  size_t interface_count() const { return this->order_.size(); }
  NetworkInterface interface_at(size_t index) const { return this->order_[index]; }
  /// Moves an interface one place earlier (delta < 0) or later (delta > 0); saved and applied at once.
  bool move_interface(NetworkInterface interface, int delta);
#ifdef NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT
  void add_active_interface_listener(ActiveInterfaceListener *listener) {
    this->active_interface_listeners_.push_back(listener);
  }
#endif

 protected:
  // Verify-lwIP-route (and its DNS servers) interval for the settled state; keeps the global lwIP core
  // mutex off the hot loop path.
  static constexpr uint32_t ROUTE_CHECK_INTERVAL_MS = 1000;
  // Last netif this component made the default; avoids redundant esp_netif calls.
  esp_netif_t *default_netif_{nullptr};
  uint32_t last_route_check_{0};
#ifdef NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT
  StaticVector<ActiveInterfaceListener *, NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT> active_interface_listeners_;
#endif
  NetworkInterface active_interface_{NetworkInterface::NETWORK_INTERFACE_NONE};
  StaticVector<NetworkInterface, NETWORK_INTERFACE_COUNT> order_;
  ESPPreferenceObject order_pref_;
#endif
};

extern NetworkComponent *global_network_component;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace esphome::network
#endif
