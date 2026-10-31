#pragma once

#include "esphome/core/defines.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_NOISE_STREAM
#include "esphome/components/noise/noise_stream.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#include "esphome/components/socket/ipv4_allow.h"
#include "esphome/components/socket/tcp_client_link.h"
#include "esphome/components/socket/tcp_listener.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_UART_VIRTUAL
#include "esphome/components/uart/uart_virtual.h"
#endif
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace esphome::uart_tcp {

#ifdef USE_UART_TCP_FORWARDS
class UartTcpForwards;
#endif

/// Copies raw bytes between one hardware UART and one TCP socket.
class UartTcp final : public Component, public uart::UARTDevice {
#ifdef USE_BINARY_SENSOR
  SUB_BINARY_SENSOR(connected)
#endif
#ifdef USE_SENSOR
  SUB_SENSOR(disconnects)
#endif

 public:
  static constexpr size_t HOST_MAX = 253;
  static constexpr size_t MAX_ALLOWED_IPS = 16;

  /// The object in PSRAM (codegen makes it so). Without PSRAM a plain one that fails its setup; no working copy
  /// in internal RAM.
  static UartTcp *create();

  // The yaml values; saved runtime settings replace them in setup().
  void set_host(const char *host) { this->link_.set_host(host); }
  void set_port(uint16_t port) { this->link_.set_port(port); }
  void set_reconnect_interval(uint32_t ms) { this->link_.set_reconnect_interval(ms); }
  void set_timeout(uint32_t ms) { this->link_.set_idle_timeout(ms); }
  void set_server(bool server) { this->server_ = server; }
  void set_allow(const socket::Ipv4AllowEntry *entries, size_t count) {
    this->yaml_allow_ = entries;
    this->yaml_allow_count_ = count;
  }
#ifdef USE_UART_VIRTUAL
  /// The UART is a virtual one: it hears of the peer coming and going and keeps what it wants on connect.
  void set_virtual_uart(uart::VirtualUARTComponent *uart) { this->virtual_uart_ = uart; }
#endif
  /// The key of the saved runtime settings, from the entry id.
  void set_settings_key(uint32_t key) { this->settings_key_ = key; }
#ifdef USE_NOISE_STREAM
  void set_noise_stream(noise::NoiseStream *stream) { this->noise_ = stream; }
#endif
#ifdef USE_NOISE_STREAM_RUNTIME_KEY
  /// Save a base64 key for `encryption:` without a key in yaml; the link reconnects with it.
  void set_encryption_key(const std::string &key) {
    if (this->noise_ != nullptr && this->noise_->set_key(this->link_, key) && this->is_enabled()) {
      this->run_(true);
    }
  }
#endif

  // Runtime settings: each applies at once and is saved; from the next boot on it replaces the yaml value.
  void set_enabled(bool enabled);
  void apply_port(uint16_t port);
  void apply_server(bool server);
  /// An IPv4 address or a hostname; false when longer than HOST_MAX.
  bool apply_host(const std::string &host);
  void apply_timeout(uint32_t ms);
  void apply_reconnect_interval(uint32_t ms);
  /// Comma separated `a.b.c.d` or `a.b.c.d/n`; empty allows every peer. False when invalid or more than
  /// MAX_ALLOWED_IPS.
  bool apply_allowed_ips(const std::string &list);

  bool is_enabled() const { return this->settings_ != nullptr && (this->settings_->flags & FLAG_DISABLED) == 0; }
  bool is_server() const { return this->server_; }
  uint16_t get_port() const { return this->link_.port(); }
  const char *get_host() const { return this->settings_ != nullptr ? this->settings_->host : ""; }
  uint32_t get_timeout() const { return this->link_.idle_timeout(); }
  uint32_t get_reconnect_interval() const { return this->link_.reconnect_interval(); }
  /// The allow list as apply_allowed_ips() takes it, cut to fit out.
  void format_allowed_ips(char *out, size_t len) const;
  /// A peer is connected (with encryption: its session is secure).
  bool is_connected() const { return this->link_was_up_; }
  /// Loop start time of the last connect.
  uint32_t connected_since() const { return this->connected_ms_; }
  size_t peer_address(std::span<char, socket::SOCKADDR_STR_LEN> buf) const { return this->link_.peer_address(buf); }
  uint32_t get_disconnects() const { return this->disconnects_; }
  /// One listener, called with true on a connect, false on a disconnect.
  void set_on_connection(void (*callback)(void *arg, bool up), void *arg) {
    this->connection_cb_ = callback;
    this->connection_arg_ = arg;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  static constexpr uint8_t FLAG_DISABLED = 1 << 0;
  static constexpr uint8_t FLAG_SERVER = 1 << 1;

  /// Saved as is (prefs, or a forwards settings file line); lives in PSRAM.
  struct Settings {
    uint32_t timeout_ms;
    uint32_t reconnect_ms;
    uint16_t port;
    uint8_t flags;
    uint8_t allow_count;
    char host[HOST_MAX + 1];
    socket::Ipv4AllowEntry allow[MAX_ALLOWED_IPS];
  };

#ifdef USE_UART_TCP_FORWARDS
  /// A runner of owner: its settings come from the owner's file, the owner drives its loop. Instead of setup().
  void setup_runner(UartTcpForwards *owner, Settings *settings);
  /// Runner: the forwarded UART, nullptr for none; the link starts again.
  void set_uart(uart::UARTComponent *uart);
  uart::UARTComponent *get_uart() const { return this->parent_; }
  /// Runner: its owner calls loop() while this holds.
  bool is_running() const { return this->running_; }
#endif

 protected:
  /// Loop on or off; a runner tells its owner instead.
  void run_(bool on);

  /// The link is up and, with encryption, its session is secure.
  bool link_up_() {
#ifdef USE_NOISE_STREAM
    if (this->noise_ != nullptr) {
      return this->noise_->up(this->link_);
    }
#endif
    return this->link_.connected();
  }
  /// Send what is queued; true once the link's buffer is empty.
  ESPHOME_ALWAYS_INLINE bool flush_link_() {
#ifdef USE_NOISE_STREAM
    if (this->noise_ != nullptr) {
      return this->noise_->flush(this->link_);
    }
#endif
    return this->link_.flush_tx();
  }
  void sync_link_(bool up);
  void read_socket_();
  void read_uart_();
  void discard_uart_();
  /// The yaml values into settings_.
  void default_settings_();
  /// settings_ into the link, the listener and the session.
  void use_settings_();
  /// The role into the session: the TCP client starts the handshake.
  void use_role_();
  /// Close the link and the listener; the next loop opens them with the current settings.
  void restart_link_();
  void save_settings_();

  static constexpr size_t READ_CHUNK = 128;
  // Scratch size for dropping stale UART bytes on connect.
  static constexpr size_t DISCARD_CHUNK = 32;

  socket::TcpClientLink link_;
  socket::TcpListener listener_;
#ifdef USE_NOISE_STREAM
  noise::NoiseStream *noise_{nullptr};
#endif
#ifdef USE_UART_VIRTUAL
  uart::VirtualUARTComponent *virtual_uart_{nullptr};
#endif
  // The runtime settings, in PSRAM; the link's host and the listener's allow list point into it.
  Settings *settings_{nullptr};
  ESPPreferenceObject pref_;
  const socket::Ipv4AllowEntry *yaml_allow_{nullptr};
  size_t yaml_allow_count_{0};
  uint32_t settings_key_{0};
#ifdef USE_UART_TCP_FORWARDS
  UartTcpForwards *owner_{nullptr};
  bool running_{false};
#endif
  // Loop start time of the last socket-to-UART write; sizes the next paced write.
  uint32_t last_write_ms_{0};
  void (*connection_cb_)(void *, bool){nullptr};
  void *connection_arg_{nullptr};
  uint32_t disconnects_{0};
  uint32_t connected_ms_{0};
  bool server_{false};
  // create() found no PSRAM: setup() fails.
  bool no_psram_{false};
  // The link state loop() saw last; edges clear the buffer and publish the sensor.
  bool link_was_up_{false};
  // A read stopped before EAGAIN. ready() stays false until new data arrives.
  bool rx_pending_{false};
};

}  // namespace esphome::uart_tcp
