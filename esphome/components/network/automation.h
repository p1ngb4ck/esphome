#pragma once
#include "esphome/core/defines.h"
#if defined(USE_NETWORK) && defined(USE_ESP32) && defined(USE_NETWORK_DEFAULT_ROUTE) && \
    defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
#include "esphome/core/automation.h"
#include "network_component.h"

namespace esphome::network {

/// on_active_interface: the interface holding the default route changed (main loop).
class ActiveInterfaceTrigger : public Trigger<>, public ActiveInterfaceListener {
 public:
  void on_active_interface(NetworkInterface interface) override { this->trigger(); }
};

}  // namespace esphome::network
#endif
