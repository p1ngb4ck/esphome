#pragma once

#include "esphome/core/defines.h"
#ifdef USE_UART_TCP_FORWARDS

#include "uart_tcp.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace esphome::uart_tcp {

/// UARTs forwarded on TCP ports at runtime. Runners (UartTcp links in PSRAM, their count saved, used from the next
/// boot) take the forwards of a settings file on a storage, local or network; each forward names one of the UARTs
/// the yaml declares, none is forwarded by default, and a UART is in one forward at most. Every change rewrites the
/// file.
class UartTcpForwards : public Component {
 public:
  static constexpr uint8_t MIN_RUNNERS = 2;
  static constexpr uint8_t MAX_RUNNERS = 12;
  static constexpr size_t PATH_MAX_LEN = 128;
  static constexpr uint16_t FIRST_PORT = 4001;

  /// The object in PSRAM. Without PSRAM a plain one that fails its setup; no working copy in internal RAM.
  static UartTcpForwards *create();
  /// The UART list, in PSRAM.
  void init_uarts(size_t count);
  void add_uart(uart::UARTComponent *uart, const char *name) {
    if (this->uart_count_ < this->uart_capacity_)
      this->uarts_[this->uart_count_++] = Uart{uart, name};
  }
  void set_runners_default(uint8_t count) { this->runners_default_ = count; }
  void set_settings_file_default(const char *path) { this->path_default_ = path; }
  void set_settings_key(uint32_t key) { this->settings_key_ = key; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// Runners made at this boot.
  uint8_t get_runner_count() const { return this->count_; }
  /// Saved; used from the next boot.
  uint8_t get_saved_runner_count() const { return this->prefs_ != nullptr ? this->prefs_->runners : 0; }
  void set_runner_count(uint8_t count);
  const char *get_settings_file() const { return this->prefs_ != nullptr ? this->prefs_->path : ""; }
  /// Saved; the forwards are read from there. False when the path is too long.
  bool set_settings_file(const std::string &path);
  /// The file was read (or found missing): changes are written from now on.
  bool is_loaded() const { return this->loaded_; }

  size_t uart_count() const { return this->uart_count_; }
  const char *uart_name(size_t index) const { return index < this->uart_count_ ? this->uarts_[index].name : ""; }
  uart::UARTComponent *uart_at(size_t index) const {
    return index < this->uart_count_ ? this->uarts_[index].uart : nullptr;
  }
  UartTcp *runner(size_t index) { return index < this->count_ ? &this->runners_[index] : nullptr; }
  /// The UART of a runner as uart_count() counts them, -1 for none.
  int get_runner_uart(size_t index) const { return index < this->count_ ? this->lines_[index].uart : -1; }
  /// -1 for none; false when another runner forwards that UART.
  bool set_runner_uart(size_t index, int uart);
  uint32_t get_baud_rate(size_t index) const { return index < this->count_ ? this->lines_[index].baud_rate : 0; }
  uint8_t get_data_bits(size_t index) const { return index < this->count_ ? this->lines_[index].data_bits : 0; }
  uart::UARTParityOptions get_parity(size_t index) const {
    return index < this->count_ ? static_cast<uart::UARTParityOptions>(this->lines_[index].parity)
                                : uart::UART_CONFIG_PARITY_NONE;
  }
  uint8_t get_stop_bits(size_t index) const { return index < this->count_ ? this->lines_[index].stop_bits : 0; }
  /// The line settings of a runner's UART, applied to it while it forwards; false when invalid.
  bool set_line_settings(size_t index, uint32_t baud_rate, uint8_t data_bits, uart::UARTParityOptions parity,
                         uint8_t stop_bits);

  /// The file is written again (a runner's setting changed).
  void save();
  /// A runner's need for the loop changed.
  void runner_changed();
  /// One listener, called when a runner connects or disconnects, or the forwards change (the file read, a setting
  /// set).
  void set_on_change(void (*callback)(void *arg), void *arg) {
    this->change_cb_ = callback;
    this->change_arg_ = arg;
  }

 protected:
  struct Uart {
    uart::UARTComponent *uart;
    const char *name;
  };
  struct Prefs {
    uint8_t runners;
    char path[PATH_MAX_LEN];
  };
  /// A runner's UART and the line settings it gets.
  struct Line {
    uint32_t baud_rate;
    int8_t uart;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
  };
  // One file line: the settings, the allow list and the host at their longest.
  static constexpr size_t LINE_SIZE = 768;
  static constexpr size_t FILE_SIZE = LINE_SIZE * MAX_RUNNERS;
  static constexpr uint32_t RETRY_MS = 500;

  void default_runner_(size_t index);
  void apply_line_(size_t index);
  void save_prefs_();
  // The settings file through the storage worker: read once, then written whole on every change, in the storage's
  // transfer buffer for the time of one read or write.
  void load_();
  void read_();
  void read_done_(storage::StorageError err);
  void parse_();
  void write_();
  void write_step_();
  void release_buffer_();

  void changed_() {
    if (this->change_cb_ != nullptr)
      this->change_cb_(this->change_arg_);
  }

  Uart *uarts_{nullptr};
  size_t uart_count_{0};
  size_t uart_capacity_{0};
  void (*change_cb_)(void *){nullptr};
  void *change_arg_{nullptr};
  UartTcp *runners_{nullptr};
  UartTcp::Settings *settings_{nullptr};
  Line *lines_{nullptr};
  Prefs *prefs_{nullptr};
  ESPPreferenceObject pref_;
  const char *path_default_{""};
  uint32_t settings_key_{0};
  storage::StreamHandle handle_{};
  uint8_t *buf_{nullptr};
  size_t buf_len_{0};
  size_t got_{0};
  uint8_t runners_default_{4};
  uint8_t count_{0};
  bool loaded_{false};
  bool busy_{false};
  bool dirty_{false};
  // Applying the file: the runners' changes are not written back.
  bool parsing_{false};
  // create() found no PSRAM: setup() fails.
  bool no_psram_{false};
};

}  // namespace esphome::uart_tcp

#endif  // USE_UART_TCP_FORWARDS
