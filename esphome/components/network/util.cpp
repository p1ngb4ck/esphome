#include "util.h"
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_NETWORK
#if defined(USE_ESP32) && defined(USE_NETWORK_DEFAULT_ROUTE)
#include "network_component.h"
#endif
#ifdef USE_NETWORK_NAME_NVS
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>
#endif

namespace esphome::network {

#if defined(USE_ESP32) && USE_NETWORK_IPV6
ESPHOME_LOG_TAG(TAG, "network");

void fill_ip6_addresses(esp_netif_t *netif, IPAddresses &addresses) {
  esp_ip6_addr_t ip6s[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
  size_t count = esp_netif_get_all_ip6(netif, ip6s);
  // Slot 0 holds the IPv4 address
  if (count > addresses.size() - 1) {
    // Warn once per interface
    static esp_netif_t *warned[3]{};
    for (auto &slot : warned) {
      if (slot == netif) {
        break;
      }
      if (slot == nullptr) {
        slot = netif;
        ESP_LOGW(TAG, "%s has %zu IPv6 addresses, only %zu are reported", esp_netif_get_desc(netif), count,
                 addresses.size() - 1);
        break;
      }
    }
    count = addresses.size() - 1;
  }
  for (size_t i = 0; i < count; i++) {
    addresses[i + 1] = IPAddress(&ip6s[i]);
  }
}
#endif

// The order of the components is important: WiFi should come after any possible main interfaces (it may be used as
// an AP that uses a previous interface for NAT).

bool is_disabled() {
  // The network is disabled only when every configured interface with a
  // disable() lifecycle is disabled; one enabled interface means traffic can flow.
  bool disabled = false;
#ifdef USE_MODEM
  if (modem::global_modem_component != nullptr) {
    if (!modem::global_modem_component->is_disabled())
      return false;
    disabled = true;
  }
#endif

#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    if (!wifi::global_wifi_component->is_disabled())
      return false;
    disabled = true;
  }
#endif

#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr) {
    if (!ethernet::global_eth_component->is_disabled())
      return false;
    disabled = true;
  }
#endif
  return disabled;
}

const char *get_use_address_to(std::span<char, USE_ADDRESS_BUFFER_SIZE> buf) {
  // Global component pointers are guaranteed to be set by component constructors when USE_* is defined.
  // A wifi-first network: priority: list sets USE_NETWORK_PRIMARY_INTERFACE_WIFI to lift
  // wifi ahead of the fixed ethernet-first order below; an ethernet-first list already
  // matches that order, so no define exists for it.
  const char *addr = nullptr;
#if defined(USE_NETWORK_PRIMARY_INTERFACE_WIFI) && defined(USE_WIFI)
  addr = wifi::global_wifi_component->get_use_address();
#elif defined(USE_ETHERNET)
  addr = ethernet::global_eth_component->get_use_address();
#elif defined(USE_MODEM)
  addr = modem::global_modem_component->get_use_address();
#elif defined(USE_WIFI)
  addr = wifi::global_wifi_component->get_use_address();
#elif defined(USE_OPENTHREAD)
  addr = openthread::global_openthread_component->get_use_address();
#endif
  if (addr != nullptr && addr[0] != '\0')
    return addr;
  // No explicit use_address configured: the address is the runtime device name
  // (which includes the MAC suffix when name_add_mac_suffix is enabled) plus ".local"
  const StringRef name = device_name();
  make_name_with_suffix_to(buf.data(), buf.size(), name.c_str(), name.size(), '.', "local", 5);
  return buf.data();
}

network::IPAddresses get_ip_addresses() {
#if defined(USE_ESP32) && defined(USE_NETWORK_DEFAULT_ROUTE)
  // The interface holding the default route, as the runtime priority order chose it
  if (global_network_component != nullptr) {
    switch (global_network_component->get_active_interface()) {
#ifdef USE_ETHERNET
      case NetworkInterface::NETWORK_INTERFACE_ETHERNET:
        return ethernet::global_eth_component->get_ip_addresses();
#endif
#ifdef USE_WIFI
      case NetworkInterface::NETWORK_INTERFACE_WIFI:
        return wifi::global_wifi_component->get_ip_addresses();
#endif
#ifdef USE_USB_ETHERNET
      case NetworkInterface::NETWORK_INTERFACE_USB_ETHERNET:
        return usb_ethernet::global_usb_ethernet->get_ip_addresses();
#endif
      default:
        break;
    }
  }
#endif
  // With a wifi-first network: priority: list, prefer wifi while it has a valid IP;
  // otherwise fall through to the fixed ethernet-first order below.
#if defined(USE_NETWORK_PRIMARY_INTERFACE_WIFI) && defined(USE_WIFI)
  if (wifi::global_wifi_component != nullptr) {
    auto ips = wifi::global_wifi_component->get_ip_addresses();
    for (const auto &ip : ips) {
      if (ip.is_set())
        return ips;
    }
  }
#endif

#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr)
    return ethernet::global_eth_component->get_ip_addresses();
#endif

#ifdef USE_MODEM
  if (modem::global_modem_component != nullptr)
    return modem::global_modem_component->get_ip_addresses();
#endif

#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr)
    return wifi::global_wifi_component->get_ip_addresses();
#endif
#ifdef USE_OPENTHREAD
  if (openthread::global_openthread_component != nullptr)
    return openthread::global_openthread_component->get_ip_addresses();
#endif
  return {};
}

#ifdef USE_NETWORK_NAME_NVS
namespace {
struct NvsNames {
  char name[32 + sizeof(NETWORK_NAME_NVS_SUFFIX)]{};
  char friendly[64 + sizeof(NETWORK_FRIENDLY_NAME_NVS_SUFFIX)]{};
  size_t name_len{0};
  size_t friendly_len{0};

  NvsNames() {
    if (nvs_flash_init() != ESP_OK)
      return;
    nvs_handle_t handle;
    if (nvs_open(NETWORK_NAME_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
      return;
    size_t len = 32;
    if (nvs_get_str(handle, NETWORK_NAME_NVS_KEY, this->name, &len) == ESP_OK && len > 1 && this->name[0] != '-') {
      bool valid = true;
      for (const char *c = this->name; *c != '\0' && valid; c++)
        valid = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-';
      if (valid) {
        std::strcat(this->name, NETWORK_NAME_NVS_SUFFIX);
        this->name_len = std::strlen(this->name);
      }
    }
    len = 64;
    if (nvs_get_str(handle, NETWORK_FRIENDLY_NAME_NVS_KEY, this->friendly, &len) == ESP_OK && len > 1) {
      std::strcat(this->friendly, NETWORK_FRIENDLY_NAME_NVS_SUFFIX);
      this->friendly_len = std::strlen(this->friendly);
    }
    nvs_close(handle);
  }
};

const NvsNames &nvs_names() {
  static const NvsNames NAMES;
  return NAMES;
}
}
#endif

StringRef device_name() {
#ifdef USE_NETWORK_NAME_NVS
  const NvsNames &names = nvs_names();
  if (names.name_len > 0)
    return StringRef(names.name, names.name_len);
#endif
  return App.get_name();
}

StringRef device_friendly_name() {
#ifdef USE_NETWORK_NAME_NVS
  const NvsNames &names = nvs_names();
  if (names.friendly_len > 0)
    return StringRef(names.friendly, names.friendly_len);
#endif
  return App.get_friendly_name();
}

}  // namespace esphome::network
#endif
