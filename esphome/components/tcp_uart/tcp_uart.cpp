#include "tcp_uart.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>
#if defined(USE_TCP_UART_MAX_CONNECTIONS) && defined(USE_NOISE_STREAM)
#include <new>
#endif

namespace esphome::tcp_uart {

ESPHOME_LOG_TAG(TAG, "tcp_uart");

static constexpr uint32_t DROP_LOG_INTERVAL_MS = 5000;

#ifdef USE_TCP_UART_MAX_CONNECTIONS
void TcpUart::set_max_connections(uint8_t count) {
  this->extra_.init(count - 1);
  for (uint8_t i = 1; i < count; i++) {
    this->extra_.emplace_back();
  }
  this->seen_ms_.init(count);
  for (uint8_t i = 0; i < count; i++) {
    this->seen_ms_.push_back(0);
  }
  // The first search for a holder starts at slot 0.
  this->last_holder_ = count - 1;
  this->listener_.set_backlog(count);
}
#endif

void TcpUart::setup() {
#if defined(USE_TCP_UART_MAX_CONNECTIONS) && defined(USE_NOISE_STREAM)
  if (this->noise_ != nullptr && !this->extra_.empty()) {
    // One session per extra connection, under slot 0's key; only a server has extra connections
    RAMAllocator<noise::NoiseStream> allocator(RAMAllocator<noise::NoiseStream>::ALLOC_EXTERNAL);
    this->extra_noise_ = allocator.allocate(this->extra_.size());
    if (this->extra_noise_ == nullptr) {
      ESP_LOGE(TAG, "No PSRAM for %u encrypted sessions", static_cast<unsigned>(this->extra_.size()));
      this->mark_failed();
      return;
    }
    for (size_t i = 0; i < this->extra_.size(); i++) {
      new (&this->extra_noise_[i]) noise::NoiseStream(this->noise_, false);
      this->extra_noise_[i].set_log_tag(TAG);
    }
  }
#endif
  this->link_.begin(TAG);
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  for (auto &link : this->extra_) {
    link.begin(TAG);
    link.set_idle_timeout(this->link_.idle_timeout());
  }
#endif
#ifdef USE_NOISE_STREAM
  if (this->noise_ != nullptr) {
    this->noise_->set_log_tag(TAG);
#ifdef USE_NOISE_STREAM_RUNTIME_KEY
    // Without a key the link stays closed: a client does not dial, a server does not listen
    if (!this->noise_->load_key()) {
      this->disable_loop();
    }
#endif
  }
#endif
#ifdef USE_SOCKET_TCP_LISTENER
  this->listener_.begin(TAG);
#endif
#ifdef USE_BINARY_SENSOR
  PUBLISH_STATE(this->connected_binary_sensor_, false);
#endif
#ifdef USE_SENSOR
  PUBLISH_STATE(this->disconnects_sensor_, 0);
#endif
}

void TcpUart::dump_config() {
  ESP_LOGCONFIG(TAG,
                "TCP UART:\n"
                "  %s: %s:%u\n"
                "  Reconnect Interval: %" PRIu32 "ms",
                this->server_ ? LOG_STR_LITERAL("Listen") : LOG_STR_LITERAL("Host"),
                this->server_ ? LOG_STR_LITERAL("*") : this->link_.host(), this->link_.port(),
                this->link_.reconnect_interval());
  if (this->link_.idle_timeout() != 0) {
    ESP_LOGCONFIG(TAG, "  Timeout: %" PRIu32 "ms", this->link_.idle_timeout());
  }
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  if (!this->extra_.empty()) {
    ESP_LOGCONFIG(TAG,
                  "  Max Connections: %u\n"
                  "  Turn Timeout: %" PRIu32 "ms",
                  this->slot_count_(), this->turn_timeout_ms_);
  }
#endif
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

void TcpUart::on_shutdown() {
  this->link_.close();
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  for (auto &link : this->extra_) {
    link.close();
  }
#endif
#ifdef USE_SOCKET_TCP_LISTENER
  this->listener_.close();
#endif
}

bool TcpUart::is_connected() {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  for (uint8_t i = 1; i < this->slot_count_(); i++) {
    if (this->session_up_(i)) {
      return true;
    }
  }
#endif
  return this->session_up_(0);
}

void TcpUart::publish_connected_(bool up) {
#ifdef USE_BINARY_SENSOR
  PUBLISH_STATE(this->connected_binary_sensor_, up);
#endif
}

void TcpUart::count_disconnect_() {
#ifdef USE_SENSOR
  if (this->disconnects_sensor_ != nullptr) {
    this->disconnects_++;
    this->disconnects_sensor_->publish_state(this->disconnects_);
  }
#endif
}

void TcpUart::sync_link_(bool up) {
  this->link_was_up_ = up;
  // Each session is a turn of its own.
  this->turn_++;
  if (up) {
    // Unread bytes of the last session stay readable while down, never into the next one.
    this->rx_start_ = this->rx_end_ = 0;
#ifdef USE_NOISE_STREAM
    // The handshake's last read can leave session bytes the socket's ready flag does not show
    if (this->noise_ != nullptr) {
      this->rx_pending_ = true;
    }
#endif
  }
  this->publish_connected_(up);
  // Only edges get here, so down is the falling edge.
  if (!up) {
    this->count_disconnect_();
  }
}

bool TcpUart::read_socket_(uint8_t i) {
  socket::TcpClientLink &link = this->slot_(i);
  if (this->rx_start_ != 0) {
    this->rx_end_ -= this->rx_start_;
    std::memmove(this->rx_, this->rx_ + this->rx_start_, this->rx_end_);
    this->rx_start_ = 0;
  }
  size_t room = RX_BUFFER_SIZE - this->rx_end_;
  if (room == 0) {
    // Unread bytes are waiting on the consumer. The peer is not idle.
    link.note_io();
    return false;
  }
#ifdef USE_NOISE_STREAM
  noise::NoiseStream *stream = this->noise_of_(i);
  ssize_t count = stream != nullptr ? stream->read(link, this->rx_ + this->rx_end_, room)
                                    : link.read(this->rx_ + this->rx_end_, room);
#else
  ssize_t count = link.read(this->rx_ + this->rx_end_, room);
#endif
  if (count <= 0) {
    // A dropped link (-1) is seen by the edge check on the next loop.
    if (count == 0) {
      this->rx_pending_ = false;
    }
    return false;
  }
  this->rx_end_ += static_cast<uint16_t>(count);
  this->rx_pending_ = static_cast<size_t>(count) == room;
  return true;
}

void TcpUart::loop() {
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  if (!this->extra_.empty()) {
    // Inline until a client connects.
    bool waiting = this->listener_.poll_listen(this->link_);
    bool active = waiting || this->up_mask_ != 0;
#ifdef USE_NOISE_STREAM
    // A handshake runs before its slot counts as up
    for (uint8_t i = 0; !active && this->noise_ != nullptr && i < this->slot_count_(); i++) {
      active = this->slot_(i).connected();
    }
#endif
    if (active) {
      this->loop_pool_(waiting);
    }
    return;
  }
#endif
#ifdef USE_SOCKET_TCP_LISTENER
  if (this->server_) {
    // link_was_up_ holds the accept until the previous drop's edge has run,
    // so the sensor sees the disconnect and the new session's edge clears RX.
    this->listener_.poll(this->link_, !this->link_was_up_);
  } else {
    this->link_.poll();
  }
#else
  this->link_.poll();
#endif
  bool up = this->link_up_(0);
  if (up != this->link_was_up_) {
    this->sync_link_(up);
  }
  if (!this->link_was_up_) {
    return;
  }
  // A byte moved in this pass resets the clock before the timeout can close.
  if (this->rx_pending_ || this->link_.ready()) {
    this->read_socket_(0);
  }
  this->flush_link_(0);
  this->link_.check_idle();
}

#ifdef USE_TCP_UART_MAX_CONNECTIONS
void TcpUart::loop_pool_(bool waiting) {
  const uint32_t now = App.get_loop_component_start_time();
  const uint8_t count = this->slot_count_();
  for (uint8_t i = 0; i < count; i++) {
    this->check_slot_(i);
  }
  if (waiting) {
    this->accept_slot_(now);
  }
  if (this->holder_ != NO_SLOT) {
    // After release_turn() only the bytes already read are left to consume.
    if (!this->release_ && (this->rx_pending_ || this->slot_(this->holder_).ready())) {
      this->read_holder_(now);
    }
    if (this->available() == 0 &&
        (this->release_ || (!this->held_ && now - this->last_traffic_ms_ >= this->turn_timeout_ms_))) {
      this->end_turn_();
    }
  }
  if (this->holder_ == NO_SLOT) {
    this->pick_holder_(now);
  }
  for (uint8_t i = 0; i < count; i++) {
    this->flush_link_(i);
    this->slot_(i).check_idle();
  }
}

void TcpUart::check_slot_(uint8_t i) {
  const bool up = this->link_up_(i);
  if (up != ((this->up_mask_ & (1u << i)) != 0)) {
    this->slot_edge_(i, up);
  }
}

void TcpUart::slot_edge_(uint8_t i, bool up) {
  const uint8_t bit = 1u << i;
  const bool was_up = this->up_mask_ != 0;
  if (up) {
    this->up_mask_ |= bit;
    this->seen_ms_[i] = App.get_loop_component_start_time();
#ifdef USE_NOISE_STREAM
    // The handshake's last read can leave session bytes the socket's ready flag does not show
    if (this->noise_of_(i) != nullptr) {
      this->pending_mask_ |= bit;
    }
#endif
  } else {
    this->up_mask_ &= ~bit;
    this->pending_mask_ &= ~bit;
    if (i == this->holder_) {
      // Its unread bytes never reach another connection.
      this->rx_start_ = this->rx_end_ = 0;
      this->rx_pending_ = false;
      this->holder_ = NO_SLOT;
      this->turn_++;
    }
    if (i == this->tx_slot_) {
      this->tx_slot_ = NO_SLOT;
    }
    this->count_disconnect_();
  }
  if ((this->up_mask_ != 0) != was_up) {
    this->publish_connected_(!was_up);
  }
}

void TcpUart::close_slot_(uint8_t i) {
  this->slot_(i).close();
#ifdef USE_NOISE_STREAM
  if (noise::NoiseStream *stream = this->noise_of_(i)) {
    stream->reset();
  }
#endif
  if ((this->up_mask_ & (1u << i)) != 0) {
    this->slot_edge_(i, false);
  }
}

void TcpUart::accept_slot_(uint32_t now) {
  auto sock = this->listener_.accept(this->link_);
  if (sock == nullptr) {
    return;
  }
  const uint8_t count = this->slot_count_();
  uint8_t slot = NO_SLOT;
  uint8_t quietest = NO_SLOT;
  uint32_t quietest_idle = 0;
  for (uint8_t i = 0; i < count; i++) {
    socket::TcpClientLink &link = this->slot_(i);
    if (!link.connected()) {
      slot = i;
      break;
    }
    if (i == this->holder_) {
      continue;
    }
    // A connection with bytes waiting for its turn is not idle.
    const bool waiting = (this->pending_mask_ & (1u << i)) != 0 || link.ready();
    const uint32_t idle = waiting ? 0 : now - this->seen_ms_[i];
    if (quietest == NO_SLOT || idle > quietest_idle) {
      quietest = i;
      quietest_idle = idle;
    }
  }
  if (slot == NO_SLOT) {
    if (quietest == NO_SLOT) {
      return;
    }
    // Modbus TCP [4.2.1.1]: a full pool closes the oldest unused connection.
    ESP_LOGI(TAG, "All %u connections in use, closing the quietest", count);
    slot = quietest;
  }
  // A new socket always starts a new session
  this->close_slot_(slot);
  this->slot_(slot).adopt(std::move(sock));
  this->check_slot_(slot);
}

void TcpUart::pick_holder_(uint32_t now) {
  const uint8_t count = this->slot_count_();
  for (uint8_t step = 1; step <= count; step++) {
    const uint8_t i = (this->last_holder_ + step) % count;
    const uint8_t bit = 1u << i;
    if ((this->up_mask_ & bit) == 0 || ((this->pending_mask_ & bit) == 0 && !this->slot_(i).ready())) {
      continue;
    }
    this->holder_ = i;
    this->tx_slot_ = i;
    this->turn_++;
    this->held_ = false;
    this->release_ = false;
    this->rx_start_ = this->rx_end_ = 0;
    this->rx_pending_ = (this->pending_mask_ & bit) != 0;
    this->pending_mask_ &= ~bit;
    this->last_traffic_ms_ = now;
    this->read_holder_(now);
    return;
  }
}

void TcpUart::read_holder_(uint32_t now) {
  if (this->read_socket_(this->holder_)) {
    this->last_traffic_ms_ = now;
    this->seen_ms_[this->holder_] = now;
  }
}

void TcpUart::end_turn_() {
  // Bytes left in the socket keep their place for the next turn.
  if (this->rx_pending_) {
    this->pending_mask_ |= 1u << this->holder_;
    this->rx_pending_ = false;
  }
  this->last_holder_ = this->holder_;
  this->holder_ = NO_SLOT;
  this->held_ = false;
  this->release_ = false;
}
#endif

void TcpUart::write_array(const uint8_t *data, size_t len) {
  const uint8_t i = this->tx_index_();
  size_t queued = 0;
  if (i != NO_SLOT) {
#ifdef USE_NOISE_STREAM
    noise::NoiseStream *stream = this->noise_of_(i);
    queued = stream != nullptr ? stream->queue(this->slot_(i), data, len) : this->slot_(i).queue(data, len);
#else
    queued = this->slot_(i).queue(data, len);
#endif
  }
#ifdef USE_TCP_UART_MAX_CONNECTIONS
  if (queued != 0 && this->holder_ != NO_SLOT) {
    this->last_traffic_ms_ = App.get_loop_component_start_time();
  }
#endif
  if (queued < len) {
    uint32_t now = App.get_loop_component_start_time();
    if (this->last_drop_log_ms_ == 0 || now - this->last_drop_log_ms_ >= DROP_LOG_INTERVAL_MS) {
      ESP_LOGW(TAG, "%s, dropped %u bytes",
               i != NO_SLOT && this->session_up_(i) ? LOG_STR_LITERAL("TX buffer full")
                                                    : LOG_STR_LITERAL("Not connected"),
               static_cast<unsigned>(len - queued));
      this->last_drop_log_ms_ = now;
    }
  }
}

bool TcpUart::peek_byte(uint8_t *data) {
  if (this->rx_start_ == this->rx_end_) {
    return false;
  }
  *data = this->rx_[this->rx_start_];
  return true;
}

bool TcpUart::read_array(uint8_t *data, size_t len) {
  if (this->available() < len) {
    return false;
  }
  std::memcpy(data, this->rx_ + this->rx_start_, len);
  this->rx_start_ += static_cast<uint16_t>(len);
  return true;
}

uart::UARTFlushResult TcpUart::flush() {
  const uint8_t i = this->tx_index_();
  if (i == NO_SLOT) {
    return uart::UARTFlushResult::UART_FLUSH_RESULT_FAILED;
  }
  bool emptied = this->flush_link_(i);
  if (!this->session_up_(i)) {
    // A down link cannot have delivered anything, whether this flush dropped
    // it or an earlier loop() write did.
    return uart::UARTFlushResult::UART_FLUSH_RESULT_FAILED;
  }
  return emptied ? uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS : uart::UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT;
}

}  // namespace esphome::tcp_uart
