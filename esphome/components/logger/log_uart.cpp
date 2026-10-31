#include "log_uart.h"
#ifdef USE_LOGGER_UART

#include "logger.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>

namespace esphome::logger {

ESPHOME_LOG_TAG(TAG, "logger.uart");

static constexpr uint8_t LINE_END[] = {'\r', '\n'};

void LogUART::attach(Logger *logger) {
  logger->add_log_callback(this, [](void *self, uint8_t level, const char *tag, const char *message, size_t len) {
    static_cast<LogUART *>(self)->on_log_(level, message, len);
  });
}

void LogUART::on_log_(uint8_t level, const char *message, size_t len) {
  if (level > this->level_ || this->rx_capacity_ - this->rx_size_ < len + sizeof(LINE_END)) {
    return;
  }
  this->inject_rx(reinterpret_cast<const uint8_t *>(message), len);
  this->inject_rx(LINE_END, sizeof(LINE_END));
}

void LogUART::setup() {
  if (this->boot_size_ != 0 && this->rx_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the boot buffer");
    this->mark_failed();
    return;
  }
  this->pref_ = global_preferences->make_preference<uint32_t>(this->settings_key_, true);
  uint32_t saved;
  if (this->pref_.load(&saved) && saved <= MAX_BOOT_HOLD_MS) {
    this->boot_hold_ms_ = saved;
  }
  // The hold counts from boot
  uint32_t now = millis();
  this->set_timeout(this->boot_hold_ms_ > now ? this->boot_hold_ms_ - now : 0, [this]() { this->end_hold_(); });
}

void LogUART::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Log UART:\n"
                "  Boot Buffer: %zu B\n"
                "  Live Buffer: %zu B\n"
                "  Boot Hold: %" PRIu32 " ms\n"
                "  Level: %u",
                this->boot_size_, this->live_size_, this->boot_hold_ms_, this->level_);
}

void LogUART::set_boot_hold(uint32_t ms) {
  ms = std::min(ms, MAX_BOOT_HOLD_MS);
  this->boot_hold_ms_ = ms;
  if (!this->pref_.save(&ms)) {
    ESP_LOGW(TAG, "Boot hold not saved");
    return;
  }
  global_preferences->sync();
}

void LogUART::end_hold_() {
  this->holding_ = false;
  if (!this->connected_) {
    // Nobody came for the boot log: its ring goes back to PSRAM
    this->free_rx_();
  }
}

void LogUART::set_reader_connected(bool connected) {
  this->connected_ = connected;
  if (connected) {
    if (this->rx_ == nullptr && this->live_size_ != 0 && !this->alloc_rx_(this->live_size_)) {
      ESP_LOGW(TAG, "No PSRAM for the live buffer");
    }
  } else if (!this->holding_) {
    this->free_rx_();
  }
}

}  // namespace esphome::logger

#endif  // USE_LOGGER_UART
