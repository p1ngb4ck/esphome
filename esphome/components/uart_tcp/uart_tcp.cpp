#include "uart_tcp.h"
#ifdef USE_UART_TCP_FORWARDS
#include "uart_tcp_forwards.h"
#endif

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/progmem.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace esphome::uart_tcp {

ESPHOME_LOG_TAG(TAG, "uart_tcp");

/// The 4 bytes of a dotted quad in network order, as Ipv4AllowEntry holds them.
static bool parse_ipv4(const char *&p, uint8_t out[4]) {
  for (int i = 0; i < 4; i++) {
    if (i != 0 && *p++ != '.') {
      return false;
    }
    if (*p < '0' || *p > '9') {
      return false;
    }
    char *end;
    unsigned long value = strtoul(p, &end, 10);
    if (value > 255 || end - p > 3) {
      return false;
    }
    out[i] = static_cast<uint8_t>(value);
    p = end;
  }
  return true;
}

static bool parse_allow_entry(const char *&p, socket::Ipv4AllowEntry &entry) {
  uint8_t addr[4];
  if (!parse_ipv4(p, addr)) {
    return false;
  }
  unsigned long prefix = 32;
  if (*p == '/') {
    p++;
    if (*p < '0' || *p > '9') {
      return false;
    }
    char *end;
    prefix = strtoul(p, &end, 10);
    if (prefix > 32) {
      return false;
    }
    p = end;
  }
  uint32_t mask_be = prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
  uint8_t mask[4] = {static_cast<uint8_t>(mask_be >> 24), static_cast<uint8_t>(mask_be >> 16),
                     static_cast<uint8_t>(mask_be >> 8), static_cast<uint8_t>(mask_be)};
  for (int i = 0; i < 4; i++) {
    addr[i] &= mask[i];
  }
  std::memcpy(&entry.addr, addr, 4);
  std::memcpy(&entry.mask, mask, 4);
  return true;
}

/// Entries of a comma separated list into out (nullptr only checks); SIZE_MAX when invalid or longer than max.
static size_t parse_allow_list(const char *p, socket::Ipv4AllowEntry *out, size_t max) {
  size_t count = 0;
  socket::Ipv4AllowEntry entry;
  while (*p == ' ') {
    p++;
  }
  while (*p != '\0') {
    if (count == max || !parse_allow_entry(p, entry)) {
      return SIZE_MAX;
    }
    if (out != nullptr) {
      out[count] = entry;
    }
    count++;
    while (*p == ' ') {
      p++;
    }
    if (*p == ',') {
      p++;
      while (*p == ' ') {
        p++;
      }
      if (*p == '\0') {
        return SIZE_MAX;
      }
    } else if (*p != '\0') {
      return SIZE_MAX;
    }
  }
  return count;
}

UartTcp *UartTcp::create() {
  void *mem = RAMAllocator<UartTcp>(RAMAllocator<UartTcp>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) UartTcp();
  auto *uart_tcp = new UartTcp();  // only to report in setup() and fail there
  uart_tcp->no_psram_ = true;
  return uart_tcp;
}

void UartTcp::setup() {
  if (this->no_psram_) {
    ESP_LOGE(TAG, "No PSRAM for the link");
    this->mark_failed();
    return;
  }
  RAMAllocator<Settings> allocator(RAMAllocator<Settings>::ALLOC_EXTERNAL);
  this->settings_ = allocator.allocate(1);
  if (this->settings_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the settings");
    this->mark_failed();
    return;
  }
  this->pref_ = global_preferences->make_preference<Settings>(this->settings_key_, true);
  if (!this->pref_.load(this->settings_) || this->settings_->port == 0 ||
      this->settings_->allow_count > MAX_ALLOWED_IPS) {
    this->default_settings_();
  }
  this->settings_->host[HOST_MAX] = '\0';
  this->use_settings_();
  this->use_role_();
  this->link_.begin(TAG);
#ifdef USE_NOISE_STREAM
  if (this->noise_ != nullptr) {
    this->noise_->set_log_tag(TAG);
#ifdef USE_NOISE_STREAM_RUNTIME_KEY
    // Without a key the link stays closed: a client does not dial, a server does not listen
    if (!this->noise_->load_key()) {
      this->run_(false);
    }
#endif
  }
#endif
  this->listener_.begin(TAG);
  if (!this->is_enabled()) {
    this->run_(false);
  }
#ifdef USE_BINARY_SENSOR
  PUBLISH_STATE(this->connected_binary_sensor_, false);
#endif
#ifdef USE_SENSOR
  PUBLISH_STATE(this->disconnects_sensor_, 0);
#endif
}

void UartTcp::dump_config() {
  ESP_LOGCONFIG(TAG,
                "UART TCP:\n"
                "  %s: %s:%u\n"
                "  Reconnect Interval: %" PRIu32 "ms",
                this->server_ ? LOG_STR_LITERAL("Listen") : LOG_STR_LITERAL("Host"),
                this->server_ ? LOG_STR_LITERAL("*") : this->link_.host(), this->link_.port(),
                this->link_.reconnect_interval());
  if (!this->is_enabled()) {
    ESP_LOGCONFIG(TAG, "  Disabled");
  }
  if (this->link_.idle_timeout() != 0) {
    ESP_LOGCONFIG(TAG, "  Timeout: %" PRIu32 "ms", this->link_.idle_timeout());
  }
#ifdef USE_NOISE_STREAM
  if (this->noise_ != nullptr) {
#ifdef USE_NOISE_STREAM_RUNTIME_KEY
    ESP_LOGCONFIG(TAG, "  Encryption: Noise%s",
                  this->noise_->has_key() ? LOG_STR_LITERAL("") : LOG_STR_LITERAL(", key: none"));
#else
    ESP_LOGCONFIG(TAG, "  Encryption: Noise");
#endif
  }
#endif
#ifdef USE_SOCKET_TCP_LISTENER
  this->listener_.dump_config();
#endif
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Connected", this->connected_binary_sensor_);
#endif
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Disconnects", this->disconnects_sensor_);
#endif
}

void UartTcp::on_shutdown() {
  this->link_.close();
#ifdef USE_SOCKET_TCP_LISTENER
  this->listener_.close();
#endif
}

void UartTcp::sync_link_(bool up) {
  this->link_was_up_ = up;
#ifdef USE_UART_VIRTUAL
  // A virtual UART decides itself what it keeps for a new peer
  const bool is_virtual = this->virtual_uart_ != nullptr;
  if (is_virtual) {
    this->virtual_uart_->set_reader_connected(up);
  }
#else
  const bool is_virtual = false;
#endif
  if (up) {
    if (!is_virtual) {
      // The driver kept whatever arrived while the link was down.
      this->discard_uart_();
    }
#ifdef USE_NOISE_STREAM
    // The handshake's last read can leave session bytes the socket's ready flag does not show
    if (this->noise_ != nullptr) {
      this->rx_pending_ = true;
    }
#endif
  }
#ifdef USE_BINARY_SENSOR
  PUBLISH_STATE(this->connected_binary_sensor_, up);
#endif
  // Only edges get here, so down is the falling edge.
  if (up) {
    this->connected_ms_ = App.get_loop_component_start_time();
  } else {
    this->disconnects_++;
#ifdef USE_SENSOR
    if (this->disconnects_sensor_ != nullptr) {
      this->disconnects_sensor_->publish_state(this->disconnects_);
    }
#endif
  }
  if (this->connection_cb_ != nullptr) {
    this->connection_cb_(this->connection_arg_, up);
  }
}

void UartTcp::read_socket_() {
  // A hardware write blocks until the driver takes every byte. Leave what does
  // not fit in the socket, so TCP flow control throttles the peer.
  size_t room = this->parent_->paced_write_room(this->last_write_ms_);
  if (room == 0) {
    // The UART cannot take more. The peer is not idle.
    this->rx_pending_ = true;
    this->link_.note_io();
    return;
  }
  uint8_t tmp[READ_CHUNK];
  size_t want = std::min(room, sizeof(tmp));
#ifdef USE_NOISE_STREAM
  ssize_t count = this->noise_ != nullptr ? this->noise_->read(this->link_, tmp, want) : this->link_.read(tmp, want);
#else
  ssize_t count = this->link_.read(tmp, want);
#endif
  if (count <= 0) {
    // A dropped link (-1) is cleaned up by sync_link_() on the next loop.
    if (count == 0) {
      this->rx_pending_ = false;
    }
    return;
  }
  this->rx_pending_ = static_cast<size_t>(count) == want;
  this->write_array(tmp, static_cast<size_t>(count));
  this->last_write_ms_ = App.get_loop_component_start_time();
}

void UartTcp::discard_uart_() {
  // Drain exactly what was buffered while the link was down; later bytes are live.
  uint8_t dump[DISCARD_CHUNK];
  size_t left = this->available();
  while (left != 0) {
    size_t n = std::min(left, sizeof(dump));
    if (!this->read_array(dump, n)) {
      return;
    }
    left -= n;
  }
}

void UartTcp::read_uart_() {
#ifdef USE_NOISE_STREAM
  if (this->noise_ != nullptr) {
    // Plaintext goes into the stream's open frame, so it cannot fill the link's buffer directly.
    uint8_t tmp[READ_CHUNK];
    for (;;) {
      size_t want = std::min({this->available(), this->noise_->tx_free(this->link_), sizeof(tmp)});
      if (want == 0 || !this->read_array(tmp, want)) {
        return;
      }
      this->noise_->queue(this->link_, tmp, want);
    }
  }
#endif
  size_t want = std::min<size_t>(this->available(), this->link_.tx_free());
  if (want != 0 && this->read_array(this->link_.tx_tail(), want)) {
    this->link_.tx_commit(want);
  }
}

void UartTcp::loop() {
#ifdef USE_SOCKET_TCP_LISTENER
  if (this->server_) {
    // link_was_up_ holds the accept until the previous drop's edge has run,
    // so the sensor and the stale UART discard always see the disconnect.
    this->listener_.poll(this->link_, !this->link_was_up_);
  } else if (this->settings_->host[0] != '\0') {
    // A server made a client at runtime has no host until one is set
    this->link_.poll();
  }
#else
  this->link_.poll();
#endif
  bool up = this->link_up_();
  if (up != this->link_was_up_) {
    this->sync_link_(up);
  }
  if (!this->link_was_up_) {
    return;
  }
  // A byte moved in this pass resets the clock before the timeout can close.
  if (this->rx_pending_ || this->link_.ready()) {
    this->read_socket_();
  }
  // UART bytes picked up here go out in the same pass.
  this->read_uart_();
  this->flush_link_();
  this->link_.check_idle();
}

void UartTcp::default_settings_() {
  Settings &s = *this->settings_;
  std::memset(&s, 0, sizeof(s));
  s.timeout_ms = this->link_.idle_timeout();
  s.reconnect_ms = this->link_.reconnect_interval();
  s.port = this->link_.port();
  s.flags = this->server_ ? FLAG_SERVER : 0;
  const char *host = this->link_.host();
  if (host != nullptr) {
    std::strncpy(s.host, host, HOST_MAX);
  }
  s.allow_count = static_cast<uint8_t>(std::min(this->yaml_allow_count_, MAX_ALLOWED_IPS));
  for (size_t i = 0; i < s.allow_count; i++) {
    progmem_memcpy(&s.allow[i], &this->yaml_allow_[i], sizeof(socket::Ipv4AllowEntry));
  }
}

void UartTcp::use_settings_() {
  const Settings &s = *this->settings_;
  this->link_.set_idle_timeout(s.timeout_ms);
  this->link_.set_reconnect_interval(s.reconnect_ms);
  this->link_.set_port(s.port);
  this->link_.set_host(s.host);
  this->listener_.set_allow(s.allow, s.allow_count);
  this->server_ = (s.flags & FLAG_SERVER) != 0;
}

void UartTcp::use_role_() {
#ifdef USE_NOISE_STREAM
  if (this->noise_ != nullptr) {
    // The TCP client starts the handshake
    this->noise_->set_initiator(!this->server_);
  }
#endif
}

void UartTcp::restart_link_() {
  this->link_.close();
  this->listener_.close();
#ifdef USE_NOISE_STREAM
  if (this->noise_ != nullptr) {
    this->noise_->reset();
  }
#endif
  if (this->link_was_up_) {
    this->sync_link_(false);
  }
}

void UartTcp::run_(bool on) {
#ifdef USE_UART_TCP_FORWARDS
  if (this->owner_ != nullptr) {
    // Nothing to forward without a UART
    this->running_ = on && this->parent_ != nullptr;
    this->owner_->runner_changed();
    return;
  }
#endif
  if (on) {
    this->enable_loop();
  } else {
    this->disable_loop();
  }
}

#ifdef USE_UART_TCP_FORWARDS
void UartTcp::setup_runner(UartTcpForwards *owner, Settings *settings) {
  this->owner_ = owner;
  this->settings_ = settings;
  this->settings_->host[HOST_MAX] = '\0';
  this->use_settings_();
  this->link_.begin(TAG);
  this->listener_.begin(TAG);
}

void UartTcp::set_uart(uart::UARTComponent *uart) {
  this->restart_link_();
  this->parent_ = uart;
  this->run_(this->is_enabled());
}
#endif

void UartTcp::save_settings_() {
#ifdef USE_UART_TCP_FORWARDS
  if (this->owner_ != nullptr) {
    this->owner_->save();
    return;
  }
#endif
  if (!this->pref_.save(this->settings_)) {
    ESP_LOGW(TAG, "Settings not saved");
    return;
  }
  global_preferences->sync();
}

void UartTcp::set_enabled(bool enabled) {
  if (this->settings_ == nullptr || enabled == this->is_enabled()) {
    return;
  }
  if (enabled) {
    this->settings_->flags &= ~FLAG_DISABLED;
#ifdef USE_NOISE_STREAM_RUNTIME_KEY
    // Without a key the link stays closed, as at boot
    if (this->noise_ == nullptr || this->noise_->has_key()) {
      this->run_(true);
    }
#else
    this->run_(true);
#endif
  } else {
    this->settings_->flags |= FLAG_DISABLED;
    this->restart_link_();
    this->run_(false);
  }
  this->save_settings_();
}

void UartTcp::apply_port(uint16_t port) {
  if (this->settings_ == nullptr || port == 0) {
    return;
  }
  this->settings_->port = port;
  this->use_settings_();
  this->restart_link_();
  this->save_settings_();
}

void UartTcp::apply_server(bool server) {
  if (this->settings_ == nullptr) {
    return;
  }
  if (server) {
    this->settings_->flags |= FLAG_SERVER;
  } else {
    this->settings_->flags &= ~FLAG_SERVER;
  }
  this->restart_link_();
  this->use_settings_();
  this->use_role_();
  this->save_settings_();
}

bool UartTcp::apply_host(const std::string &host) {
  if (this->settings_ == nullptr || host.size() > HOST_MAX) {
    return false;
  }
  std::memcpy(this->settings_->host, host.c_str(), host.size() + 1);
  this->use_settings_();
  if (!this->server_) {
    this->restart_link_();
  }
  this->save_settings_();
  return true;
}

void UartTcp::apply_timeout(uint32_t ms) {
  if (this->settings_ == nullptr) {
    return;
  }
  this->settings_->timeout_ms = ms;
  this->use_settings_();
  this->save_settings_();
}

void UartTcp::apply_reconnect_interval(uint32_t ms) {
  if (this->settings_ == nullptr) {
    return;
  }
  this->settings_->reconnect_ms = ms;
  this->use_settings_();
  this->save_settings_();
}

bool UartTcp::apply_allowed_ips(const std::string &list) {
  if (this->settings_ == nullptr) {
    return false;
  }
  // The first pass checks the list, the second writes it, so a refused list leaves the old one
  if (parse_allow_list(list.c_str(), nullptr, MAX_ALLOWED_IPS) == SIZE_MAX) {
    ESP_LOGW(TAG, "Invalid allow list");
    return false;
  }
  this->settings_->allow_count =
      static_cast<uint8_t>(parse_allow_list(list.c_str(), this->settings_->allow, MAX_ALLOWED_IPS));
  this->use_settings_();
  // A connected peer the new list refuses is dropped
  if (this->server_) {
    this->restart_link_();
  }
  this->save_settings_();
  return true;
}

void UartTcp::format_allowed_ips(char *out, size_t len) const {
  if (len == 0) {
    return;
  }
  out[0] = '\0';
  if (this->settings_ == nullptr) {
    return;
  }
  size_t pos = 0;
  for (size_t i = 0; i < this->settings_->allow_count && pos < len; i++) {
    const socket::Ipv4AllowEntry &e = this->settings_->allow[i];
    uint8_t addr[4];
    std::memcpy(addr, &e.addr, 4);
    int prefix = __builtin_popcount(e.mask);
    int n = snprintf(out + pos, len - pos, "%s%u.%u.%u.%u/%d", i == 0 ? "" : ",", addr[0], addr[1], addr[2], addr[3],
                     prefix);
    if (n < 0) {
      return;
    }
    pos += static_cast<size_t>(n);
  }
}

}  // namespace esphome::uart_tcp
