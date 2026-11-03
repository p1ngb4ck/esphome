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
#include "esphome/components/socket/tcp_client_link.h"
#ifdef USE_SOCKET_TCP_LISTENER
#include "esphome/components/socket/tcp_listener.h"
#endif
#include "esphome/components/uart/uart_component.h"
#include "esphome/core/component.h"
#ifdef USE_TCP_UART_MAX_CONNECTIONS
#include "esphome/core/helpers.h"
#endif

#include <cstdint>

namespace esphome::tcp_uart {

/// TCP client or server presented as a UART. Bytes are copied unchanged.
class TcpUart : public uart::UARTComponent, public Component {
#ifdef USE_BINARY_SENSOR
  SUB_BINARY_SENSOR(connected)
#endif
#ifdef USE_SENSOR
  SUB_SENSOR(disconnects)
#endif

 public:
  TcpUart() { this->rx_buffer_size_ = RX_BUFFER_SIZE; }

  void set_host(const char *host) { this->link_.set_host(host); }
  void set_port(uint16_t port) { this->link_.set_port(port); }
  void set_reconnect_interval(uint32_t ms) { this->link_.set_reconnect_interval(ms); }
  void set_timeout(uint32_t ms) { this->link_.set_idle_timeout(ms); }
#ifdef USE_NOISE_STREAM
  void set_noise_stream(noise::NoiseStream *stream) { this->noise_ = stream; }
#endif
#ifdef USE_NOISE_STREAM_RUNTIME_KEY
  /// Save a base64 key for `encryption:` without a key in yaml; every connection reconnects with it.
  void set_encryption_key(const std::string &key) {
    if (this->noise_ != nullptr && this->noise_->set_key(this->link_, key)) {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
      for (auto &link : this->extra_) {
        link.close();
      }
#endif
      this->enable_loop();
    }
  }
#endif
#ifdef USE_SOCKET_TCP_LISTENER
  void set_server(bool server) { this->server_ = server; }
#ifdef USE_SOCKET_IPV4_ALLOW
  void set_allow(const socket::Ipv4AllowEntry *entries, size_t count) { this->listener_.set_allow(entries, count); }
#endif
#endif
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  /// Server: open this many connections at once. Call once, before setup().
  void set_max_connections(uint8_t count);
  void set_turn_timeout(uint32_t ms) { this->turn_timeout_ms_ = ms; }
#endif

  /// Changes whenever another connection, or a new session, gets the turn.
  /// Reads come from that connection only, writes go to it.
  uint16_t turn() const { return this->turn_; }
  /// Keeps the turn on the current connection until release_turn().
  void hold_turn() {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
    this->held_ = true;
    this->release_ = false;
#endif
  }
  /// Ends the turn as soon as the bytes already read from its connection are consumed.
  void release_turn() {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
    this->held_ = false;
    this->release_ = true;
#endif
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override { return static_cast<size_t>(this->rx_end_ - this->rx_start_); }
  // Same room write_array() grants, so consumers can apply backpressure.
  size_t available_for_write() override {
    const uint8_t i = this->tx_index_();
    if (i == NO_SLOT) {
      return 0;
    }
#ifdef USE_NOISE_STREAM
    if (noise::NoiseStream *stream = this->noise_of_(i)) {
      return stream->tx_free(this->slot_(i));
    }
#endif
    return this->slot_(i).tx_free();
  }
  uart::UARTFlushResult flush() override;
  bool is_connected() override;
#if defined(USE_ESP8266) || defined(USE_ESP32)
  void load_settings(bool dump_config) override {}
  using UARTComponent::load_settings;  // also bring in the no-arg overload for convenience
#endif

 protected:
  static constexpr uint8_t NO_SLOT = 0xFF;

  void check_logger_conflict() override {}
  /// Slot 0 is link_; slots 1 and up are the extra connections of a server.
  socket::TcpClientLink &slot_(uint8_t i) {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
    if (i != 0) {
      return this->extra_[i - 1];
    }
#endif
    return this->link_;
  }
#ifdef USE_NOISE_STREAM
  /// The slot's session, or nullptr without encryption.
  noise::NoiseStream *noise_of_(uint8_t i) const {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
    if (i != 0) {
      return this->extra_noise_ == nullptr ? nullptr : &this->extra_noise_[i - 1];
    }
#endif
    return this->noise_;
  }
#endif
  /// The slot's link is up and, with encryption, its session is secure.
  bool link_up_(uint8_t i) {
#ifdef USE_NOISE_STREAM
    if (noise::NoiseStream *stream = this->noise_of_(i)) {
      return stream->up(this->slot_(i));
    }
#endif
    return this->slot_(i).connected();
  }
  /// The state link_up_() last reached, without driving the handshake.
  bool session_up_(uint8_t i) {
#ifdef USE_NOISE_STREAM
    if (noise::NoiseStream *stream = this->noise_of_(i)) {
      return stream->ready() && this->slot_(i).connected();
    }
#endif
    return this->slot_(i).connected();
  }
  /// Send what is queued; true once the slot's buffer is empty.
  ESPHOME_ALWAYS_INLINE bool flush_link_(uint8_t i) {
#ifdef USE_NOISE_STREAM
    if (noise::NoiseStream *stream = this->noise_of_(i)) {
      return stream->flush(this->slot_(i));
    }
#endif
    return this->slot_(i).flush_tx();
  }
  /// The slot that writes go to, or NO_SLOT.
  uint8_t tx_index_() const {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
    if (!this->extra_.empty()) {
      return this->tx_slot_;
    }
#endif
    return 0;
  }
  void sync_link_(bool up);
  /// True when bytes arrived.
  bool read_socket_(uint8_t i);
  void publish_connected_(bool up);
  void count_disconnect_();
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  uint8_t slot_count_() const { return static_cast<uint8_t>(this->extra_.size() + 1); }
  void loop_pool_(bool waiting);
  /// Runs the slot's edge when its session state changed.
  void check_slot_(uint8_t i);
  void slot_edge_(uint8_t i, bool up);
  /// Close a slot and forget its session, before its link takes another socket.
  void close_slot_(uint8_t i);
  void accept_slot_(uint32_t now);
  void pick_holder_(uint32_t now);
  void read_holder_(uint32_t now);
  void end_turn_();
#endif

  static constexpr size_t RX_BUFFER_SIZE = 1024;

  socket::TcpClientLink link_;
#ifdef USE_SOCKET_TCP_LISTENER
  socket::TcpListener listener_;
#endif
#ifdef USE_NOISE_STREAM
  // Slot 0's session; it owns the key.
  noise::NoiseStream *noise_{nullptr};
#endif
#ifdef USE_SENSOR
  uint32_t disconnects_{0};
#endif
  uint32_t last_drop_log_ms_{0};
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  // Slots 1 and up; slot 0 is link_, which also holds the port and the listener's retry clock.
  FixedVector<socket::TcpClientLink> extra_;
#ifdef USE_NOISE_STREAM
  // Sessions of slots 1 and up under noise_'s key, in PSRAM; nullptr without encryption.
  noise::NoiseStream *extra_noise_{nullptr};
#endif
  // Per slot: when it connected or last sent bytes. A full pool closes the quietest.
  FixedVector<uint32_t> seen_ms_;
  // Last byte in either direction on the holder's connection.
  uint32_t last_traffic_ms_{0};
  uint32_t turn_timeout_ms_{100};
  uint8_t holder_{NO_SLOT};
  // Writes go here: the holder, else the last holder until it closes.
  uint8_t tx_slot_{NO_SLOT};
  // The search for the next holder starts after this slot.
  uint8_t last_holder_{0};
  // Slots loop() saw up (with encryption: session secure).
  uint8_t up_mask_{0};
  // Slots whose last read stopped before EAGAIN; ready() stays false for them.
  uint8_t pending_mask_{0};
  bool held_{false};
  bool release_{false};
#endif
  // rx_[rx_start_, rx_end_) holds unread bytes; read_socket_() compacts to the front.
  uint16_t rx_start_{0};
  uint16_t rx_end_{0};
  uint16_t turn_{0};
  bool server_{false};
  // The link state loop() saw last; edges publish the sensor, the up edge clears rx_.
  bool link_was_up_{false};
  // A read stopped before EAGAIN. ready() stays false until new data arrives.
  bool rx_pending_{false};
  uint8_t rx_[RX_BUFFER_SIZE]{};
};

}  // namespace esphome::tcp_uart
