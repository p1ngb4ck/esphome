#include "improv_serial_component.h"
#ifdef USE_IMPROV_SERIAL
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include "esphome/components/logger/logger.h"
#include "esphome/components/network/util.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/scan_list.h"
#endif

#include <array>

#ifdef IMPROV_SERIAL_IDENTITY_NAMESPACE
#include <cstdlib>
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>
#endif
#if defined(USE_API) && defined(USE_API_NOISE) && !defined(USE_API_NOISE_PSK_FROM_YAML)
#include "esphome/components/api/api_server.h"
#endif

namespace esphome::improv_serial {

ESPHOME_LOG_TAG(TAG, "improv_serial");

static constexpr uint32_t WIFI_CONNECT_TIMEOUT_ID = 0;
static constexpr uint8_t RPC_GET_NETWORK_CONFIG = 0x80;
static constexpr uint8_t RPC_SET_NETWORK_PRIORITY = 0x81;
static constexpr uint8_t RPC_SET_INTERFACE_ENABLED = 0x82;
static constexpr uint8_t RPC_SET_ETHERNET_IP = 0x83;
static constexpr uint8_t RPC_SET_WIFI_NETWORK = 0x84;
#ifdef IMPROV_SERIAL_IDENTITY_NAMESPACE
static constexpr uint8_t RPC_GET_IDENTITY = 0x85;
static constexpr uint8_t RPC_SET_IDENTITY = 0x86;
static constexpr uint8_t RPC_GET_MQTT = 0x87;
static constexpr uint8_t RPC_SET_MQTT = 0x88;
static constexpr uint8_t RPC_GET_AP = 0x89;
static constexpr uint8_t RPC_SET_AP = 0x8A;
static constexpr uint8_t RPC_SET_API_KEY = 0x8B;
static constexpr size_t IDENTITY_NAME_MAX = 24;
static constexpr size_t IDENTITY_FRIENDLY_NAME_MAX = 63;
static constexpr size_t IDENTITY_MQTT_BROKER_MAX = 63;
static constexpr size_t IDENTITY_MQTT_PORT_MAX = 5;
static constexpr size_t IDENTITY_MQTT_USER_MAX = 63;
static constexpr size_t IDENTITY_SECRET_MAX = 63;
static constexpr size_t IDENTITY_AP_NAME_MAX = 32;

static bool identity_read_(const char *key, std::string &out) {
  out.clear();
  nvs_flash_init();
  nvs_handle_t handle;
  if (nvs_open(IMPROV_SERIAL_IDENTITY_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return false;
  size_t len = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &len);
  if (err == ESP_OK && len > 0) {
    out.resize(len);
    err = nvs_get_str(handle, key, out.data(), &len);
    if (err == ESP_OK)
      out.resize(len > 0 ? len - 1 : 0);
    else
      out.clear();
  }
  nvs_close(handle);
  return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

static bool identity_write_(const char *key, const std::string &value) {
  nvs_handle_t handle;
  if (nvs_open(IMPROV_SERIAL_IDENTITY_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  esp_err_t err = value.empty() ? nvs_erase_key(handle, key) : nvs_set_str(handle, key, value.c_str());
  if (err == ESP_ERR_NVS_NOT_FOUND)
    err = ESP_OK;
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}

static bool identity_port_valid_(const std::string &text) {
  if (text.empty())
    return true;
  if (text.size() > IDENTITY_MQTT_PORT_MAX)
    return false;
  for (const char c : text)
    if (c < '0' || c > '9')
      return false;
  const long port = strtol(text.c_str(), nullptr, 10);
  return port >= 1 && port <= 65535;
}
#endif

void ImprovSerialComponent::setup() {
  global_improv_serial_component = this;
#ifdef USE_IMPROV_SERIAL_UART
  // Transport is a dedicated UART bus set via set_uart() in generated code
#elif defined(USE_ESP32)
  this->uart_num_ = logger::global_logger->get_uart_num();
  this->uart_selection_ = logger::global_logger->get_uart();
#elif defined(USE_ARDUINO)
  this->hw_serial_ = logger::global_logger->get_hw_serial();
#endif

  // The Improv state machine tracks Wi-Fi provisioning only. General device
  // connectivity (e.g. Ethernet) is reported separately via GET_NETWORK_STATE.
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr && wifi::global_wifi_component->has_sta()) {
    this->state_ = improv::STATE_PROVISIONED;
  } else if (wifi::global_wifi_component != nullptr && !wifi::global_wifi_component->is_disabled()) {
    // Respect Wi-Fi's disabled state; forcing a scan while disabled throws
    // the wifi component into an invalid state from which it cannot recover.
    wifi::global_wifi_component->start_scanning();
  }
#endif
}

void ImprovSerialComponent::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (this->last_read_byte_ && (now - this->last_read_byte_ > IMPROV_SERIAL_TIMEOUT)) {
    this->last_read_byte_ = 0;
    this->rx_buffer_.clear();
    ESP_LOGV(TAG, "Timeout");
  }

  while (true) {
    auto byte = this->read_byte_();
    if (!byte.has_value())
      break;
    if (this->parse_improv_serial_byte_(byte.value())) {
      this->last_read_byte_ = now;
    } else {
      this->last_read_byte_ = 0;
      this->rx_buffer_.clear();
    }
  }

#ifdef USE_WIFI
  if (this->state_ == improv::STATE_PROVISIONING && wifi::global_wifi_component != nullptr &&
      wifi::global_wifi_component->is_connected()) {
    // Being connected is not enough: re-provisioning a device that is already online leaves the
    // prior network up until it drops, so check that the joined network is the requested one
    // before reporting success. Same test as the wifi.connect action.
    char ssid_buf[wifi::SSID_BUFFER_SIZE];
    if (strcmp(wifi::global_wifi_component->wifi_ssid_to(ssid_buf), this->connecting_sta_.get_ssid().c_str()) == 0) {
      wifi::global_wifi_component->save_wifi_sta(this->connecting_sta_.get_ssid(),
                                                 this->connecting_sta_.get_password());
      this->connecting_sta_ = {};
      this->cancel_timeout(WIFI_CONNECT_TIMEOUT_ID);
      this->set_state_(improv::STATE_PROVISIONED);

      this->send_settings_response_(improv::WIFI_SETTINGS);
    }
  }
#endif
}

void ImprovSerialComponent::dump_config() { ESP_LOGCONFIG(TAG, "Improv Serial:"); }

void ImprovSerialComponent::write_data_(const uint8_t *data, const size_t size) {
  // First, set length field
  this->tx_header_[TX_LENGTH_IDX] = this->tx_header_[TX_TYPE_IDX] == TYPE_RPC_RESPONSE ? size : 1;

  const bool there_is_data = data != nullptr && size > 0;
  // If there_is_data, checksum must not include our optional data byte
  const uint8_t header_checksum_len = there_is_data ? TX_BUFFER_SIZE - 3 : TX_BUFFER_SIZE - 2;
  // Only transmit the full buffer length if there is no data (only state/error byte is provided in this case)
  const uint8_t header_tx_len = there_is_data ? TX_BUFFER_SIZE - 3 : TX_BUFFER_SIZE;
  uint8_t checksum = sum_bytes(this->tx_header_, header_checksum_len);
  if (there_is_data) {
    checksum = sum_bytes(data, size, checksum);
  }
  this->tx_header_[TX_CHECKSUM_IDX] = checksum;

#ifdef USE_IMPROV_SERIAL_UART
  this->uart_->write_array(this->tx_header_, header_tx_len);
  if (there_is_data) {
    this->uart_->write_array(data, size);
    this->uart_->write_array(&this->tx_header_[TX_CHECKSUM_IDX], 2);  // Footer: checksum and newline
  }
#elif defined(USE_ESP32)
  switch (this->uart_selection_) {
    case logger::UART_SELECTION_UART0:
    case logger::UART_SELECTION_UART1:
#if defined(USE_ESP32_VARIANT_ESP32)
    case logger::UART_SELECTION_UART2:
#endif
      uart_write_bytes(this->uart_num_, this->tx_header_, header_tx_len);
      if (there_is_data) {
        uart_write_bytes(this->uart_num_, data, size);
        uart_write_bytes(this->uart_num_, &this->tx_header_[TX_CHECKSUM_IDX], 2);  // Footer: checksum and newline
      }
      break;
#if defined(USE_LOGGER_USB_CDC) && defined(CONFIG_ESP_CONSOLE_USB_CDC)
    case logger::UART_SELECTION_USB_CDC:
      esp_usb_console_write_buf((const char *) this->tx_header_, header_tx_len);
      if (there_is_data) {
        esp_usb_console_write_buf((const char *) data, size);
        esp_usb_console_write_buf((const char *) &this->tx_header_[TX_CHECKSUM_IDX],
                                  2);  // Footer: checksum and newline
      }
      break;
#endif
#ifdef USE_LOGGER_USB_SERIAL_JTAG
    case logger::UART_SELECTION_USB_SERIAL_JTAG:
      usb_serial_jtag_write_bytes((const char *) this->tx_header_, header_tx_len, 20 / portTICK_PERIOD_MS);
      if (there_is_data) {
        usb_serial_jtag_write_bytes((const char *) data, size, 20 / portTICK_PERIOD_MS);
        usb_serial_jtag_write_bytes((const char *) &this->tx_header_[TX_CHECKSUM_IDX], 2,
                                    20 / portTICK_PERIOD_MS);  // Footer: checksum and newline
      }
      break;
#endif
    default:
      break;
  }
#elif defined(USE_ARDUINO)
  this->hw_serial_->write(this->tx_header_, header_tx_len);
  if (there_is_data) {
    this->hw_serial_->write(data, size);
    this->hw_serial_->write(&this->tx_header_[TX_CHECKSUM_IDX], 2);  // Footer: checksum and newline
  }
#endif
}

#ifdef USE_WEBSERVER
void ImprovSerialComponent::add_webserver_urls_(improv::RpcResponseBuilder &builder, [[maybe_unused]] bool wifi_first) {
  // The webserver listens on every interface, so advertise each one that has a usable IPv4.
  // network::get_ip_addresses() can't be used here: it returns only the highest-priority
  // interface's addresses, which are all-unset (0.0.0.0) when e.g. Ethernet has no link while
  // the device is online via Wi-Fi, and 0.0.0.0 must not become the advertised URL. OpenThread
  // is omitted: it only ever has IPv6 addresses, which cannot form an IPv4 http:// URL.
  const auto append_urls = [&builder](const network::IPAddresses &addresses) {
    for (const auto &ip : addresses) {
      if (!ip.is_ip4() || !ip.is_set())
        continue;
      char ip_buf[network::IP_ADDRESS_BUFFER_SIZE];
      ip.str_to(ip_buf);
      // "http://" (7) + IP (40) + ":" (1) + port (5) + null (1) = 54
      char webserver_url[7 + network::IP_ADDRESS_BUFFER_SIZE + 1 + 5 + 1];
      // buf_append_printf keeps the format string in flash on ESP8266
      size_t len =
          buf_append_printf(webserver_url, sizeof(webserver_url), 0, "http://%s:%u", ip_buf, USE_WEBSERVER_PORT);
      if (!builder.add_string(webserver_url, len)) {
        ESP_LOGW(TAG, "Response full; URL dropped");
      }
    }
  };
#ifdef USE_WIFI
  // Clients redirect to the first URL, so the interface the client just configured has to lead:
  // another interface's address can be on a subnet that client cannot reach.
  const auto append_wifi_urls = [&append_urls]() {
    if (wifi::global_wifi_component != nullptr)
      append_urls(wifi::global_wifi_component->get_ip_addresses());
  };
  if (wifi_first)
    append_wifi_urls();
#endif
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr)
    append_urls(ethernet::global_eth_component->get_ip_addresses());
#endif
#ifdef USE_MODEM
  if (modem::global_modem_component != nullptr)
    append_urls(modem::global_modem_component->get_ip_addresses());
#endif
#ifdef USE_WIFI
  if (!wifi_first)
    append_wifi_urls();
#endif
}
#endif  // USE_WEBSERVER

void ImprovSerialComponent::send_settings_response_(improv::Command command) {
  std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
  improv::RpcResponseBuilder builder(buf, command);
#ifdef USE_IMPROV_NEXT_URL
  this->add_next_url_(builder, MAX_NEXT_URL_LEN);
#endif
#ifdef USE_WEBSERVER
  // This response only ever answers Wi-Fi provisioning, so lead with the Wi-Fi URL as it did
  // before other interfaces were reported.
  this->add_webserver_urls_(builder, /*wifi_first=*/true);
#endif
  this->send_response_(builder.finish(false));
}

void ImprovSerialComponent::send_version_info_() {
// Entry cost per field is sizeof(lit): a length byte plus the string
#ifdef ESPHOME_PROJECT_NAME
  static constexpr size_t INFO_ENTRIES_LEN =
      sizeof(ESPHOME_PROJECT_NAME) + sizeof(ESPHOME_PROJECT_VERSION) + sizeof(ESPHOME_VARIANT);
#else
  static constexpr size_t INFO_ENTRIES_LEN = sizeof("ESPHome") + sizeof(ESPHOME_VERSION) + sizeof(ESPHOME_VARIANT);
#endif
  static_assert(INFO_ENTRIES_LEN < MAX_SERIAL_PAYLOAD,
                "esphome project name and version too long for the improv_serial device info frame");
  std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
  improv::RpcResponseBuilder builder(buf, improv::GET_DEVICE_INFO);
#ifdef USE_ESP8266
  // Keep each literal in flash and copy it through an exact size stack buffer,
  // so a long project name or version can never be truncated
#define IMPROV_ADD_INFO(lit) \
  do { \
    static const char progmem_str[] PROGMEM = lit; \
    char tmp[sizeof(lit)]; \
    progmem_memcpy(tmp, progmem_str, sizeof(lit)); \
    builder.add_string(tmp, sizeof(lit) - 1); \
  } while (0)
#else
  // Literals are directly flash mapped on all other platforms
#define IMPROV_ADD_INFO(lit) builder.add_string(lit, sizeof(lit) - 1)
#endif
#ifdef ESPHOME_PROJECT_NAME
  IMPROV_ADD_INFO(ESPHOME_PROJECT_NAME);
  IMPROV_ADD_INFO(ESPHOME_PROJECT_VERSION);
#else
  IMPROV_ADD_INFO("ESPHome");
  IMPROV_ADD_INFO(ESPHOME_VERSION);
#endif
  IMPROV_ADD_INFO(ESPHOME_VARIANT);
#undef IMPROV_ADD_INFO
  // Only the device name length is unknown at compile time
#ifdef USE_NETWORK
  const StringRef name = network::device_name();
#else
  const auto &name = App.get_name();
#endif
  if (INFO_ENTRIES_LEN + 1 + name.size() <= MAX_SERIAL_PAYLOAD) {
    builder.add_string(name.c_str(), name.size());
  } else {
    ESP_LOGW(TAG, "Response full; device name dropped");
  }
  this->send_response_(builder.finish(false));
}

bool ImprovSerialComponent::parse_improv_serial_byte_(uint8_t byte) {
  size_t at = this->rx_buffer_.size();
  this->rx_buffer_.push_back(byte);
  ESP_LOGV(TAG, "Byte: 0x%02X", byte);
  const uint8_t *raw = &this->rx_buffer_[0];

  return improv::parse_improv_serial_byte(
      at, byte, raw, [this](improv::ImprovCommand command) -> bool { return this->parse_improv_payload_(command); },
      [this](improv::Error error) -> void {
        ESP_LOGW(TAG, "Error decoding payload");
        this->set_error_(error);
      });
}

#ifdef USE_WIFI
void ImprovSerialComponent::provision_(const wifi::WiFiAP &sta) {
  this->connecting_sta_ = sta;

  // Sampled before start_connecting(): the old connection drops asynchronously after it.
  const bool switching = wifi::global_wifi_component->is_connected();
  wifi::global_wifi_component->set_sta(sta);
  wifi::global_wifi_component->start_connecting(sta);
  this->set_state_(improv::STATE_PROVISIONING);

  this->set_timeout(WIFI_CONNECT_TIMEOUT_ID, switching ? WIFI_SWITCH_TIMEOUT_MS : WIFI_CONNECT_TIMEOUT_MS,
                    [this]() { this->on_wifi_connect_timeout_(); });
}
#endif

std::vector<std::string> ImprovSerialComponent::rpc_strings_() const {
  std::vector<std::string> out;
  if (this->rx_buffer_.size() < 11)
    return out;
  const size_t end = std::min<size_t>(this->rx_buffer_.size(), 11 + this->rx_buffer_[10]);
  size_t pos = 11;
  while (pos < end) {
    const size_t len = this->rx_buffer_[pos++];
    if (pos + len > end)
      break;
    out.emplace_back(reinterpret_cast<const char *>(&this->rx_buffer_[pos]), len);
    pos += len;
  }
  return out;
}

void ImprovSerialComponent::send_ack_(uint8_t code) {
  std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
  improv::RpcResponseBuilder builder(buf, static_cast<improv::Command>(code));
  this->send_response_(builder.finish(false));
}

bool ImprovSerialComponent::network_rpc_(uint8_t code) {
  const std::vector<std::string> args = this->rpc_strings_();
  auto ip_of = [](const std::string &text, network::IPAddress &out) -> bool {
    out = network::IPAddress(text);
    return out.is_set() && out.is_ip4();
  };
  auto optional_ip = [](const std::string &text, network::IPAddress &out) -> bool {
    out = network::IPAddress(text);
    return text.empty() || (out.is_set() && out.is_ip4());
  };
  switch (code) {
    case RPC_GET_NETWORK_CONFIG: {
      std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
      improv::RpcResponseBuilder builder(buf, static_cast<improv::Command>(code));
      char ip[network::IP_ADDRESS_BUFFER_SIZE];
      auto add_ip = [&builder, &ip](const network::IPAddress &address) {
        builder.add_string(address.is_set() ? address.str_to(ip) : "");
      };
      std::vector<network::NetworkInterface> order;
#ifdef USE_NETWORK_DEFAULT_ROUTE
      if (network::global_network_component != nullptr) {
        for (size_t i = 0; i < network::global_network_component->interface_count(); i++)
          order.push_back(network::global_network_component->interface_at(i));
      }
#else
#ifdef USE_ETHERNET
      order.push_back(network::NetworkInterface::NETWORK_INTERFACE_ETHERNET);
#endif
#ifdef USE_WIFI
      order.push_back(network::NetworkInterface::NETWORK_INTERFACE_WIFI);
#endif
#endif
      for (const auto interface : order) {
#ifdef USE_ETHERNET
        if (interface == network::NetworkInterface::NETWORK_INTERFACE_ETHERNET) {
          auto *eth = ethernet::global_eth_component;
          const auto &manual = eth->get_manual_ip();
          builder.add_string("ethernet");
          builder.add_string(eth->is_enabled() ? "1" : "0");
          builder.add_string(manual.has_value() ? "static" : "dhcp");
          if (manual.has_value()) {
            add_ip(manual->static_ip);
            add_ip(manual->subnet);
            add_ip(manual->gateway);
            add_ip(manual->dns1);
            add_ip(manual->dns2);
          } else {
            for (int n = 0; n < 5; n++)
              builder.add_string("");
          }
          continue;
        }
#endif
#ifdef USE_WIFI
        if (interface == network::NetworkInterface::NETWORK_INTERFACE_WIFI) {
          builder.add_string("wifi");
          builder.add_string(wifi::global_wifi_component->is_disabled() ? "0" : "1");
          optional<wifi::ManualIP> manual{};
#ifdef USE_WIFI_SAVED_NETWORKS
          if (wifi::global_wifi_component->network_count() > 0)
            manual = wifi::global_wifi_component->network_at(0).get_manual_ip();
#endif
          builder.add_string(manual.has_value() ? "static" : "dhcp");
          if (manual.has_value()) {
            add_ip(manual->static_ip);
            add_ip(manual->subnet);
            add_ip(manual->gateway);
            add_ip(manual->dns1);
            add_ip(manual->dns2);
          } else {
            for (int n = 0; n < 5; n++)
              builder.add_string("");
          }
        }
#endif
      }
      this->send_response_(builder.finish(false));
      return true;
    }
    case RPC_SET_NETWORK_PRIORITY: {
#ifdef USE_NETWORK_DEFAULT_ROUTE
      auto *net = network::global_network_component;
      if (net == nullptr) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      for (size_t target = 0; target < args.size(); target++) {
        const auto interface = args[target] == "ethernet" ? network::NetworkInterface::NETWORK_INTERFACE_ETHERNET
                                                          : network::NetworkInterface::NETWORK_INTERFACE_WIFI;
        for (size_t guard = 0; guard < net->interface_count(); guard++) {
          size_t at = 0;
          while (at < net->interface_count() && net->interface_at(at) != interface)
            at++;
          if (at >= net->interface_count() || at <= target)
            break;
          net->move_interface(interface, -1);
        }
      }
#endif
      this->send_ack_(code);
      return true;
    }
    case RPC_SET_INTERFACE_ENABLED: {
      if (args.size() < 2) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      const bool on = args[1] == "1";
#ifdef USE_ETHERNET
      if (args[0] == "ethernet")
        ethernet::global_eth_component->set_enabled(on);
#endif
#if defined(USE_WIFI) && defined(USE_WIFI_SAVED_NETWORKS)
      if (args[0] == "wifi")
        wifi::global_wifi_component->set_enabled(on);
#endif
      this->send_ack_(code);
      return true;
    }
    case RPC_SET_ETHERNET_IP: {
#ifdef USE_ETHERNET
      if (args.empty()) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      if (args[0] != "static") {
        ethernet::global_eth_component->apply_manual_ip({});
        this->send_ack_(code);
        return true;
      }
      ethernet::ManualIP manual{};
      if (args.size() < 4 || !ip_of(args[1], manual.static_ip) || !ip_of(args[2], manual.subnet) ||
          !ip_of(args[3], manual.gateway) || !optional_ip(args.size() > 4 ? args[4] : "", manual.dns1) ||
          !optional_ip(args.size() > 5 ? args[5] : "", manual.dns2)) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      ethernet::global_eth_component->apply_manual_ip(manual);
#endif
      this->send_ack_(code);
      return true;
    }
    case RPC_SET_WIFI_NETWORK: {
#ifdef USE_WIFI
      if (args.size() < 4 || args[0].empty() || wifi::global_wifi_component == nullptr ||
          wifi::global_wifi_component->is_disabled()) {
        this->set_error_(args.size() < 4 || args[0].empty() ? improv::ERROR_INVALID_RPC
                                                             : improv::ERROR_UNABLE_TO_CONNECT);
        return true;
      }
      wifi::WiFiAP sta{};
      sta.set_ssid(args[0].c_str());
      sta.set_password(args[1].c_str());
      sta.set_hidden(args[2] == "1");
      if (args[3] == "static") {
        wifi::ManualIP manual{};
        if (args.size() < 7 || !ip_of(args[4], manual.static_ip) || !ip_of(args[5], manual.subnet) ||
            !ip_of(args[6], manual.gateway) || !optional_ip(args.size() > 7 ? args[7] : "", manual.dns1) ||
            !optional_ip(args.size() > 8 ? args[8] : "", manual.dns2)) {
          this->set_error_(improv::ERROR_INVALID_RPC);
          return true;
        }
        sta.set_manual_ip(manual);
      }
      ESP_LOGD(TAG, "Received network settings: SSID=%s", args[0].c_str());
      this->provision_(sta);
#else
      this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
#endif
      return true;
    }
    default:
      this->set_error_(improv::ERROR_UNKNOWN_RPC);
      return false;
  }
}

#ifdef IMPROV_SERIAL_IDENTITY_NAMESPACE
bool ImprovSerialComponent::identity_rpc_(uint8_t code) {
  const std::vector<std::string> args = this->rpc_strings_();
  auto stored = [](const char *key) {
    std::string value;
    identity_read_(key, value);
    return value;
  };
  switch (code) {
    case RPC_GET_IDENTITY: {
      std::string name = stored("name");
      if (name.empty())
        name = App.get_name().str();
      std::string friendly_name = stored("friendly_name");
      if (friendly_name.empty())
        friendly_name = App.get_friendly_name().str();
      bool key_set = false;
#if defined(USE_API) && defined(USE_API_NOISE) && defined(USE_OTA_ENCRYPTION_PROVISIONED) && \
    !defined(USE_API_NOISE_PSK_FROM_YAML)
      noise::psk_t psk;
      key_set = api::load_saved_noise_psk(psk);
#endif
      std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
      improv::RpcResponseBuilder builder(buf, static_cast<improv::Command>(code));
      if (!builder.add_string(name.c_str(), name.size()) ||
          !builder.add_string(friendly_name.c_str(), friendly_name.size()) ||
          !builder.add_string(key_set ? "1" : "0", 1)) {
        this->set_error_(improv::ERROR_UNKNOWN);
        return true;
      }
      this->send_response_(builder.finish(false));
      return true;
    }
    case RPC_SET_IDENTITY: {
      if (args.size() < 2 || args[0].size() > IDENTITY_NAME_MAX ||
          args[1].size() > IDENTITY_FRIENDLY_NAME_MAX ||
          !identity_write_("name", args[0]) ||
          !identity_write_("friendly_name", args[1])) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      this->send_ack_(code);
      return true;
    }
    case RPC_GET_MQTT: {
      const std::string broker = stored("mqtt_broker");
      const std::string port = stored("mqtt_port");
      const std::string user = stored("mqtt_user");
      const bool password_set = !stored("mqtt_pw").empty();
      std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
      improv::RpcResponseBuilder builder(buf, static_cast<improv::Command>(code));
      if (!builder.add_string(broker.c_str(), broker.size()) ||
          !builder.add_string(port.c_str(), port.size()) ||
          !builder.add_string(user.c_str(), user.size()) ||
          !builder.add_string(password_set ? "1" : "0", 1)) {
        this->set_error_(improv::ERROR_UNKNOWN);
        return true;
      }
      this->send_response_(builder.finish(false));
      return true;
    }
    case RPC_SET_MQTT: {
      if (args.size() < 4 || args[0].size() > IDENTITY_MQTT_BROKER_MAX ||
          args[2].size() > IDENTITY_MQTT_USER_MAX || args[3].size() > IDENTITY_SECRET_MAX ||
          !identity_port_valid_(args[1]) ||
          !identity_write_("mqtt_broker", args[0]) ||
          !identity_write_("mqtt_port", args[1]) ||
          !identity_write_("mqtt_user", args[2]) ||
          (!args[3].empty() && !identity_write_("mqtt_pw", args[3]))) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      this->send_ack_(code);
      return true;
    }
    case RPC_GET_AP: {
      const std::string enable = stored("ap_enable") == "off" ? "off" : "on";
      const std::string name = stored("ap_name");
      const std::string popup = stored("ap_popup") == "off" ? "off" : "on";
      const bool password_set = !stored("ap_pw").empty();
      std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
      improv::RpcResponseBuilder builder(buf, static_cast<improv::Command>(code));
      if (!builder.add_string(enable.c_str(), enable.size()) ||
          !builder.add_string(name.c_str(), name.size()) ||
          !builder.add_string(popup.c_str(), popup.size()) ||
          !builder.add_string(password_set ? "1" : "0", 1)) {
        this->set_error_(improv::ERROR_UNKNOWN);
        return true;
      }
      this->send_response_(builder.finish(false));
      return true;
    }
    case RPC_SET_AP: {
      if (args.size() < 4 || (args[0] != "on" && args[0] != "off") ||
          args[1].size() > IDENTITY_AP_NAME_MAX || args[2].size() > IDENTITY_SECRET_MAX ||
          (args[3] != "on" && args[3] != "off") ||
          !identity_write_("ap_enable", args[0]) ||
          !identity_write_("ap_name", args[1]) ||
          (!args[2].empty() && !identity_write_("ap_pw", args[2])) ||
          !identity_write_("ap_popup", args[3])) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      this->send_ack_(code);
      return true;
    }
    case RPC_SET_API_KEY: {
#if defined(USE_API) && defined(USE_API_NOISE) && !defined(USE_API_NOISE_PSK_FROM_YAML)
      uint8_t decoded[32];
      if (args.empty() || args[0].empty() ||
          base64_decode(args[0], decoded, sizeof(decoded)) != sizeof(decoded)) {
        this->set_error_(improv::ERROR_INVALID_RPC);
        return true;
      }
      noise::psk_t psk{};
      memcpy(psk.data(), decoded, sizeof(decoded));
      if (api::global_api_server == nullptr || !api::global_api_server->save_noise_psk(psk)) {
        this->set_error_(improv::ERROR_UNKNOWN);
        return true;
      }
      this->send_ack_(code);
#else
      this->set_error_(improv::ERROR_UNKNOWN_RPC);
#endif
      return true;
    }
    default:
      this->set_error_(improv::ERROR_UNKNOWN_RPC);
      return false;
  }
}
#endif

bool ImprovSerialComponent::parse_improv_payload_(improv::ImprovCommand &command) {
  const auto code = static_cast<uint8_t>(command.command);
#ifdef IMPROV_SERIAL_IDENTITY_NAMESPACE
  if (code >= RPC_GET_IDENTITY && command.command != improv::BAD_CHECKSUM)
    return this->identity_rpc_(code);
#endif
  if (code >= RPC_GET_NETWORK_CONFIG && command.command != improv::BAD_CHECKSUM)
    return this->network_rpc_(code);
  switch (command.command) {
    case improv::WIFI_SETTINGS: {
#ifdef USE_WIFI
      if (wifi::global_wifi_component == nullptr || wifi::global_wifi_component->is_disabled()) {
        // Wi-Fi is disabled, so we can't provision. Respond immediately
        // instead of letting the client wait out its provisioning timeout.
        ESP_LOGW(TAG, "Wi-Fi is disabled; cannot provision");
        this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
        return true;
      }
      wifi::WiFiAP sta{};
      sta.set_ssid(command.ssid.c_str());
      sta.set_password(command.password.c_str());
      ESP_LOGD(TAG, "Received settings: SSID=%s, password=" LOG_SECRET("%s"), command.ssid.c_str(),
               command.password.c_str());
      this->provision_(sta);
#else
      // No Wi-Fi support compiled in; there is nothing to provision.
      ESP_LOGW(TAG, "Wi-Fi not supported; cannot provision");
      this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
#endif
      return true;
    }
    case improv::GET_CURRENT_STATE: {
      // This state machine tracks Wi-Fi provisioning only. When Wi-Fi is disabled or not
      // compiled in, provisioning is unavailable -> report STOPPED so the client doesn't
      // offer a Wi-Fi form. General connectivity (e.g. Ethernet) is reported separately
      // via GET_NETWORK_STATE.
#ifdef USE_WIFI
      if (wifi::global_wifi_component == nullptr || wifi::global_wifi_component->is_disabled()) {
        // Reported transiently without disturbing our internal provisioning state machine,
        // so a later `wifi.enable` still reports the correct state.
        this->send_current_state_(improv::STATE_STOPPED);
        return true;
      }
      this->set_state_(this->state_);
      if (this->state_ == improv::STATE_PROVISIONED) {
        this->send_settings_response_(improv::GET_CURRENT_STATE);
      }
#else
      this->send_current_state_(improv::STATE_STOPPED);
#endif
      return true;
    }
    case improv::GET_DEVICE_INFO: {
      this->send_version_info_();
      return true;
    }
    case improv::GET_WIFI_NETWORKS: {
      // Declared out here because the terminating empty response is sent with or without Wi-Fi
      std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
#ifdef USE_WIFI
      const auto &results = wifi::global_wifi_component->get_scan_result();
      for (const auto &scan : results) {
        bool with_auth = false;
        if (!wifi::should_show_scan_entry(results, scan, with_auth))
          continue;
        // Send each ssid separately to avoid overflowing the buffer
        char rssi_buf[5];  // int8_t: -128 to 127, max 4 chars + null
        char *rssi_end = int8_to_str(rssi_buf, scan.get_rssi());
        *rssi_end = '\0';
        improv::RpcResponseBuilder builder(buf, improv::GET_WIFI_NETWORKS);
        // SSID(32) + RSSI(4) + YESNO(3) entries always fit the payload
        const auto &ssid = scan.get_ssid();
        builder.add_string(ssid.c_str(), ssid.size());
        builder.add_string(rssi_buf, rssi_end - rssi_buf);
        builder.add_string(YESNO(with_auth));
        this->send_response_(builder.finish(false));
      }
#endif  // USE_WIFI
      // Send empty response to signify the end of the list.
      improv::RpcResponseBuilder builder(buf, improv::GET_WIFI_NETWORKS);
      this->send_response_(builder.finish(false));
      return true;
    }
    case improv::GET_NETWORK_STATE: {
      // Reports general device connectivity and which network interfaces are present, decoupled
      // from the Wi-Fi-only provisioning state machine. data[0] is a decimal flags byte;
      // when online, the reachable device URL(s) follow.
      uint8_t flags = 0;
      if (network::is_connected())
        flags |= improv::NETWORK_IS_ONLINE;
#ifdef USE_WIFI
      flags |= improv::NETWORK_SUPPORTS_WIFI;
#endif
#ifdef USE_ETHERNET
      flags |= improv::NETWORK_SUPPORTS_ETHERNET;
#endif
#ifdef USE_OPENTHREAD
      flags |= improv::NETWORK_SUPPORTS_THREAD;
#endif
#ifdef USE_MODEM
      flags |= improv::NETWORK_SUPPORTS_MODEM;
#endif
      std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
      improv::RpcResponseBuilder builder(buf, improv::GET_NETWORK_STATE);
      // Every flag bit fits int8_t's positive range, so int8_to_str renders the byte
      static_assert(improv::NETWORK_SUPPORTS_MODEM <= 0x7F, "network flags no longer fit int8_to_str");
      char flags_buf[4];  // uint8_t: max "255" + null
      char *flags_end = int8_to_str(flags_buf, static_cast<int8_t>(flags));
      builder.add_string(flags_buf, flags_end - flags_buf);
#ifdef USE_WEBSERVER
      // Not tied to one interface, so follow the configured priority the way
      // network::get_ip_addresses() does: a wifi-first network priority list leads with Wi-Fi.
      if (flags & improv::NETWORK_IS_ONLINE) {
#if defined(USE_NETWORK_PRIMARY_INTERFACE_WIFI) && defined(USE_WIFI)
        this->add_webserver_urls_(builder, /*wifi_first=*/true);
#else
        this->add_webserver_urls_(builder, /*wifi_first=*/false);
#endif
      }
#endif
      this->send_response_(builder.finish(false));
      return true;
    }
    default: {
      ESP_LOGW(TAG, "Unknown payload");
      this->set_error_(improv::ERROR_UNKNOWN_RPC);
      return false;
    }
  }
}

void ImprovSerialComponent::set_state_(improv::State state) {
  this->state_ = state;
  this->send_current_state_(state);
}

void ImprovSerialComponent::send_current_state_(improv::State state) {
  this->tx_header_[TX_TYPE_IDX] = TYPE_CURRENT_STATE;
  this->tx_header_[TX_DATA_IDX] = state;
  this->write_data_();
}

void ImprovSerialComponent::set_error_(improv::Error error) {
  this->tx_header_[TX_TYPE_IDX] = TYPE_ERROR_STATE;
  this->tx_header_[TX_DATA_IDX] = error;
  this->write_data_();
}

void ImprovSerialComponent::send_response_(std::span<const uint8_t> response) {
  // The serial frame length field is a single byte
  if (response.size() > MAX_SERIAL_RESPONSE) {
    ESP_LOGE(TAG, "Response too long");
    // Fail fast instead of leaving the client to wait out its timeout
    this->set_error_(improv::ERROR_UNKNOWN);
    return;
  }
  this->tx_header_[TX_TYPE_IDX] = TYPE_RPC_RESPONSE;
  this->write_data_(response.data(), response.size());
}

#ifdef USE_WIFI
void ImprovSerialComponent::on_wifi_connect_timeout_() {
  this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
  this->set_state_(improv::STATE_AUTHORIZED);
  ESP_LOGW(TAG, "Timed out while connecting to Wi-Fi network");
  wifi::global_wifi_component->clear_sta();
}
#endif

ImprovSerialComponent *global_improv_serial_component =  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    nullptr;                                             // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::improv_serial

#endif
