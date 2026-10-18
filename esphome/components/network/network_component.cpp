#include "network_component.h"

#include "esphome/core/defines.h"
#if defined(USE_NETWORK) && defined(USE_ESP32)
#include "esphome/core/log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/opt.h"

#ifdef USE_NETWORK_DEFAULT_ROUTE
#include <algorithm>
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#endif

namespace esphome::network {

ESPHOME_LOG_TAG(TAG, "network");

NetworkComponent *global_network_component = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

NetworkComponent::NetworkComponent() { global_network_component = this; }

#ifdef USE_NETWORK_DEFAULT_ROUTE
struct SavedNetworkOrder {
  uint8_t count;
  uint8_t order[NETWORK_INTERFACE_COUNT];
} PACKED;
#endif

void NetworkComponent::setup() {
  // Initialize ESP-IDF network interfaces and ensure the default event loop exists
  esp_err_t err;
  err = esp_netif_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init failed: (%d) %s", err, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  err = esp_event_loop_create_default();
  // ESP_ERR_INVALID_STATE is returned if the default loop already exists,
  // which is fine since we just want to make sure it exists
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_event_loop_create_default failed: (%d) %s", err, esp_err_to_name(err));
    this->mark_failed();
    return;
  }
#ifdef USE_NETWORK_DEFAULT_ROUTE
  // A saved order is taken only when it holds exactly the configured interfaces
  this->order_pref_ = global_preferences->make_preference<SavedNetworkOrder>(fnv1_hash("network_order"), true);
  SavedNetworkOrder saved{};
  if (this->order_pref_.load(&saved) && saved.count == this->order_.size()) {
    StaticVector<NetworkInterface, NETWORK_INTERFACE_COUNT> order;
    for (size_t i = 0; i < saved.count; i++) {
      auto interface = static_cast<NetworkInterface>(saved.order[i]);
      if (std::find(this->order_.begin(), this->order_.end(), interface) == this->order_.end() ||
          std::find(order.begin(), order.end(), interface) != order.end())
        break;
      order.push_back(interface);
    }
    if (order.size() == this->order_.size())
      this->order_ = order;
  }
#endif
}

void NetworkComponent::dump_config() {
  // The effective compile-time lwIP value, so the log reflects tcp_send_buffer
  // or the high performance bundle when either changed it.
  ESP_LOGCONFIG(TAG,
                "Network:\n"
                "  TCP send buffer: %d bytes",
                TCP_SND_BUF);
}

#ifdef USE_NETWORK_DEFAULT_ROUTE
static esp_netif_t *connected_wifi_netif() {
#ifdef USE_WIFI
  auto *wifi = wifi::global_wifi_component;
  if (wifi != nullptr && wifi->is_connected())
    return wifi->get_esp_netif_sta();
#endif
  return nullptr;
}

static esp_netif_t *connected_ethernet_netif() {
#ifdef USE_ETHERNET
  auto *eth = ethernet::global_eth_component;
  if (eth != nullptr && eth->is_connected())
    return eth->get_esp_netif();
#endif
  return nullptr;
}

static esp_netif_t *connected_netif(NetworkInterface interface) {
  switch (interface) {
    case NetworkInterface::NETWORK_INTERFACE_ETHERNET:
      return connected_ethernet_netif();
    case NetworkInterface::NETWORK_INTERFACE_WIFI:
      return connected_wifi_netif();
    default:
      return nullptr;
  }
}

bool NetworkComponent::move_interface(NetworkInterface interface, int delta) {
  auto it = std::find(this->order_.begin(), this->order_.end(), interface);
  if (it == this->order_.end())
    return false;
  const int from = static_cast<int>(it - this->order_.begin());
  const int to = from + delta;
  if (delta == 0 || to < 0 || to >= static_cast<int>(this->order_.size()))
    return false;
  this->order_[from] = this->order_[to];
  this->order_[to] = interface;
  SavedNetworkOrder saved{};
  saved.count = static_cast<uint8_t>(this->order_.size());
  for (size_t i = 0; i < this->order_.size(); i++)
    saved.order[i] = static_cast<uint8_t>(this->order_[i]);
  this->order_pref_.save(&saved);
  global_preferences->sync();
  ESP_LOGI(TAG, "Interface priority changed");
  return true;
}

void NetworkComponent::loop() {
  // Pin the default route to the first connected interface in the priority order
  // (order_); ESP-IDF's own route_prio selection would always favor WiFi.
  esp_netif_t *best = nullptr;
  NetworkInterface active = NetworkInterface::NETWORK_INTERFACE_NONE;
  for (NetworkInterface interface : this->order_) {
    best = connected_netif(interface);
    if (best != nullptr) {
      active = interface;
      break;
    }
  }
  if (active != this->active_interface_) {
    this->active_interface_ = active;
#ifdef NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT
    for (auto *listener : this->active_interface_listeners_)
      listener->on_active_interface(active);
#endif
  }
  if (best == nullptr) {
    // Forget the last winner: stopping its netif cleared lwIP's default route and
    // IDF's manual override suppresses re-election, so reconnect must re-assert it.
    this->default_netif_ = nullptr;
    return;
  }
  if (best == this->default_netif_) {
    // Same winner as the last assert. Still re-assert if lwIP's default route is
    // not the winner's netif: a winner whose netif bounced down and up between two
    // polls would otherwise stay routeless (stopping a netif nulls lwIP's
    // netif_default). Checking lwIP directly keeps this independent of IDF's
    // re-election bookkeeping (esp_netif_get_default_netif() cannot detect it).
    // Throttled: LwIPLock is the global lwIP core mutex, and this branch runs on
    // every pass once the route has settled.
    const uint32_t now = App.get_loop_component_start_time();
    if (now - this->last_route_check_ < ROUTE_CHECK_INTERVAL_MS)
      return;
    this->last_route_check_ = now;
    bool route_is_ours;
    {
      LwIPLock lock;
      route_is_ours = static_cast<void *>(netif_default) == esp_netif_get_netif_impl(best);
    }
    if (route_is_ours)
      return;
  }
  esp_err_t err = esp_netif_set_default_netif(best);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set default interface: (%d) %s", err, esp_err_to_name(err));
    // Cache the intent anyway: subsequent passes take the same-winner branch
    // above, so retries are throttled to ROUTE_CHECK_INTERVAL_MS and the lwIP
    // verification keeps re-attempting until the route is actually ours.
    this->default_netif_ = best;
    this->last_route_check_ = App.get_loop_component_start_time();
    return;
  }
  this->default_netif_ = best;
  ESP_LOGI(TAG, "Default interface: %s", esp_netif_get_desc(best));
}
#endif  // USE_NETWORK_DEFAULT_ROUTE

}  // namespace esphome::network
#endif
