#pragma once

#include "uart_component.h"
#include "esphome/core/helpers.h"

#include <cstddef>
#include <cstdint>

namespace esphome::uart {

/// Receive half of a UART without a wire. The derived class feeds the reader with inject_rx() and does the writing.
/// Reads never wait; a short read takes nothing. Main loop only.
class VirtualUARTComponent : public UARTComponent {
 public:
  /// The RX ring is allocated here, in PSRAM where there is PSRAM; rx_capacity() is 0 when that failed.
  explicit VirtualUARTComponent(size_t rx_buffer_size);

  /// Adds one whole block to the RX ring; false, and nothing kept, when it does not fit.
  bool inject_rx(const uint8_t *data, size_t len);
  size_t rx_capacity() const { return this->rx_capacity_; }
  /// The reader on the other side (a socket bridge) got or lost its peer.
  virtual void set_reader_connected(bool connected) {}

  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override { return this->rx_size_; }
#if defined(USE_ESP8266) || defined(USE_ESP32)
  using UARTComponent::load_settings;
  // Nothing is clocked, so there is nothing to apply.
  void load_settings(bool dump_config) override {}
#endif

 protected:
  void check_logger_conflict() override {}
  /// For write_array() of the derived class: hands the written bytes to a configured UART debugger.
  void debug_tx_(const uint8_t *data, size_t len) {
#ifdef USE_UART_DEBUGGER
    for (size_t i = 0; i < len; i++)
      this->debug_callback_.call(UART_DIRECTION_TX, data[i], StringRef());
#endif
  }
  /// Replace the RX ring by an empty one of size bytes (0: none); false when it could not be allocated.
  bool alloc_rx_(size_t size);
  void free_rx_() { this->alloc_rx_(0); }

  uint8_t *rx_{nullptr};
  size_t rx_capacity_{0};
  size_t rx_head_{0};
  size_t rx_size_{0};
};

}  // namespace esphome::uart
