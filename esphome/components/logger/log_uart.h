#pragma once

#include "esphome/core/defines.h"
#ifdef USE_LOGGER_UART

#include "esphome/components/uart/uart_virtual.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <cstddef>
#include <cstdint>

namespace esphome::logger {

class Logger;

/// The log as the receive side of a UART, for a socket bridge such as uart_tcp. From the logger's first line on
/// it keeps the boot log in its boot ring until a reader connects or the boot hold runs out without one; then
/// the ring is freed, and a reader that connects later gets a live ring for as long as it stays. A line that
/// does not fit is dropped whole. Bytes written to it are dropped.
class LogUART : public uart::VirtualUARTComponent, public Component {
 public:
  LogUART(size_t boot_buffer_size, size_t live_buffer_size)
      : VirtualUARTComponent(boot_buffer_size), boot_size_(boot_buffer_size), live_size_(live_buffer_size) {}
  /// The object in PSRAM. Without PSRAM a plain one that fails its setup; no working copy in internal RAM.
  static LogUART *create(size_t boot_buffer_size, size_t live_buffer_size);

  /// Register with the logger; call before Logger::pre_setup() so its first line is kept.
  void attach(Logger *logger);
  void set_level(uint8_t level) { this->level_ = level; }
  uint8_t get_level() const { return this->level_; }
  /// The yaml boot hold, before setup().
  void set_boot_hold_default(uint32_t ms) { this->boot_hold_ms_ = ms; }
  void set_settings_key(uint32_t key) { this->settings_key_ = key; }
  /// Saved; takes effect from the next boot.
  void set_boot_hold(uint32_t ms);
  uint32_t get_boot_hold() const { return this->boot_hold_ms_; }

  enum class BufferState : uint8_t { BOOT, LIVE, NONE };
  /// What the ring in PSRAM holds now: the boot log, live lines, or there is none.
  BufferState buffer_state() const {
    return this->rx_ == nullptr ? BufferState::NONE : this->boot_ring_ ? BufferState::BOOT : BufferState::LIVE;
  }

  void setup() override;
  void dump_config() override;

  void write_array(const uint8_t *data, size_t len) override { this->debug_tx_(data, len); }
  size_t available_for_write() override { return WRITE_ROOM; }
  uart::UARTFlushResult flush() override { return uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS; }
  void set_reader_connected(bool connected) override;

  static constexpr uint32_t MAX_BOOT_HOLD_MS = 60000;

 protected:
  // Room offered to a writer; what it writes is dropped.
  static constexpr size_t WRITE_ROOM = 1024;

  void on_log_(uint8_t level, const char *message, size_t len);
  void end_hold_();

  ESPPreferenceObject pref_;
  size_t boot_size_;
  size_t live_size_;
  uint32_t boot_hold_ms_{10000};
  uint32_t settings_key_{0};
  uint8_t level_{0};
  bool holding_{true};
  // The ring is still the boot ring.
  bool boot_ring_{true};
  bool connected_{false};
  // create() found no PSRAM: setup() fails.
  bool no_psram_{false};
};

}  // namespace esphome::logger

#endif  // USE_LOGGER_UART
