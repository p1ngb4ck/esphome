#include "uart_tcp_forwards.h"
#ifdef USE_UART_TCP_FORWARDS

#include "esphome/core/log.h"
#ifdef USE_STORAGE_TRANSFER_BUFFER
#include "esphome/components/storage/transfer_buffer.h"
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace esphome::uart_tcp {

ESPHOME_LOG_TAG(TAG, "uart_tcp.forwards");

using storage::StorageError;

static constexpr uint32_t DEFAULT_RECONNECT_MS = 5000;
static constexpr uint32_t DEFAULT_BAUD_RATE = 115200;
// Parity in the file, by UARTParityOptions
static constexpr char PARITY_CHARS[] = "NEOMS";

void UartTcpForwards::setup() {
  this->prefs_ = RAMAllocator<Prefs>(RAMAllocator<Prefs>::ALLOC_EXTERNAL).allocate(1);
  if (this->prefs_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the settings");
    this->mark_failed();
    return;
  }
  this->pref_ = global_preferences->make_preference<Prefs>(this->settings_key_, true);
  if (!this->pref_.load(this->prefs_) || this->prefs_->runners < MIN_RUNNERS || this->prefs_->runners > MAX_RUNNERS) {
    this->prefs_->runners = this->runners_default_;
    std::snprintf(this->prefs_->path, PATH_MAX_LEN, "%s", this->path_default_);
  }
  this->prefs_->path[PATH_MAX_LEN - 1] = '\0';
  this->count_ = this->prefs_->runners;

  this->runners_ = RAMAllocator<UartTcp>(RAMAllocator<UartTcp>::ALLOC_EXTERNAL).allocate(this->count_);
  this->settings_ =
      RAMAllocator<UartTcp::Settings>(RAMAllocator<UartTcp::Settings>::ALLOC_EXTERNAL).allocate(this->count_);
  this->lines_ = RAMAllocator<Line>(RAMAllocator<Line>::ALLOC_EXTERNAL).allocate(this->count_);
  if (this->runners_ == nullptr || this->settings_ == nullptr || this->lines_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for %u runners", this->count_);
    this->count_ = 0;
    this->mark_failed();
    return;
  }
  for (size_t i = 0; i < this->count_; i++) {
    new (&this->runners_[i]) UartTcp();
    this->default_runner_(i);
    this->runners_[i].setup_runner(this, &this->settings_[i]);
    this->runners_[i].add_on_connection_callback([this](bool) { this->change_callback_.call(); });
  }
  this->disable_loop();
#ifndef USE_STORAGE_TRANSFER_BUFFER
  ESP_LOGE(TAG, "The settings file needs the storage's transfer buffer (psram transfer buffer)");
#else
  if (storage::global_storage_registry != nullptr) {
    // The file's storage may come later (a card put in, a network storage once the network is up)
    storage::global_storage_registry->add_on_registered_callback([this](storage::Storage *) {
      if (!this->loaded_) {
        this->load_();
      } else if (this->dirty_) {
        this->write_();
      }
    });
  }
  this->load_();
#endif
}

void UartTcpForwards::default_runner_(size_t index) {
  UartTcp::Settings &s = this->settings_[index];
  std::memset(&s, 0, sizeof(s));
  s.port = static_cast<uint16_t>(FIRST_PORT + index);
  s.reconnect_ms = DEFAULT_RECONNECT_MS;
  // Not forwarded until set up
  s.flags = UartTcp::FLAG_SERVER | UartTcp::FLAG_DISABLED;
  this->lines_[index] = Line{DEFAULT_BAUD_RATE, -1, 8, uart::UART_CONFIG_PARITY_NONE, 1};
}

void UartTcpForwards::loop() {
  for (size_t i = 0; i < this->count_; i++) {
    if (this->runners_[i].is_running()) {
      this->runners_[i].loop();
    }
  }
}

void UartTcpForwards::runner_changed() {
  for (size_t i = 0; i < this->count_; i++) {
    if (this->runners_[i].is_running()) {
      this->enable_loop();
      return;
    }
  }
  this->disable_loop();
}

void UartTcpForwards::dump_config() {
  ESP_LOGCONFIG(TAG,
                "UART TCP forwards:\n"
                "  Runners: %u (saved: %u)\n"
                "  Settings file: %s\n"
                "  UARTs: %u",
                this->count_, this->get_saved_runner_count(), this->get_settings_file(),
                static_cast<unsigned>(this->uarts_.size()));
}

void UartTcpForwards::on_shutdown() {
  for (size_t i = 0; i < this->count_; i++) {
    this->runners_[i].on_shutdown();
  }
}

void UartTcpForwards::save_prefs_() {
  if (!this->pref_.save(this->prefs_)) {
    ESP_LOGW(TAG, "Settings not saved");
    return;
  }
  global_preferences->sync();
}

void UartTcpForwards::set_runner_count(uint8_t count) {
  if (this->prefs_ == nullptr || count < MIN_RUNNERS || count > MAX_RUNNERS) {
    return;
  }
  this->prefs_->runners = count;
  this->save_prefs_();
}

bool UartTcpForwards::set_settings_file(const std::string &path) {
  if (this->prefs_ == nullptr || path.size() >= PATH_MAX_LEN) {
    return false;
  }
  std::memcpy(this->prefs_->path, path.c_str(), path.size() + 1);
  this->save_prefs_();
  // The forwards come from the new file; one not there yet is made from the current ones
  this->loaded_ = false;
  this->dirty_ = true;
  this->load_();
  return true;
}

bool UartTcpForwards::set_runner_uart(size_t index, int uart) {
  if (index >= this->count_ || uart < -1 || uart >= static_cast<int>(this->uarts_.size())) {
    return false;
  }
  for (size_t i = 0; uart >= 0 && i < this->count_; i++) {
    if (i != index && this->lines_[i].uart == uart) {
      return false;
    }
  }
  this->lines_[index].uart = static_cast<int8_t>(uart);
  this->apply_line_(index);
  this->runners_[index].set_uart(uart >= 0 ? this->uarts_[uart].uart : nullptr);
  this->save();
  return true;
}

bool UartTcpForwards::set_line_settings(size_t index, uint32_t baud_rate, uint8_t data_bits,
                                        uart::UARTParityOptions parity, uint8_t stop_bits) {
  if (index >= this->count_ || baud_rate == 0 || data_bits < 5 || data_bits > 8 ||
      parity > uart::UART_CONFIG_PARITY_SPACE || (stop_bits != 1 && stop_bits != 2)) {
    return false;
  }
  Line &line = this->lines_[index];
  line.baud_rate = baud_rate;
  line.data_bits = data_bits;
  line.parity = static_cast<uint8_t>(parity);
  line.stop_bits = stop_bits;
  this->apply_line_(index);
  this->save();
  return true;
}

void UartTcpForwards::apply_line_(size_t index) {
  const Line &line = this->lines_[index];
  if (line.uart < 0) {
    return;
  }
  uart::UARTComponent *u = this->uarts_[line.uart].uart;
  u->set_baud_rate(line.baud_rate);
  u->set_data_bits(line.data_bits);
  u->set_parity(static_cast<uart::UARTParityOptions>(line.parity));
  u->set_stop_bits(line.stop_bits);
#if defined(USE_ESP8266) || defined(USE_ESP32)
  u->load_settings(false);
#endif
}

void UartTcpForwards::save() {
  if (this->parsing_) {
    return;
  }
  this->change_callback_.call();
  this->dirty_ = true;
  this->write_();
}

void UartTcpForwards::release_buffer_() {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  if (this->buf_ != nullptr) {
    storage::global_transfer_buffer->release();
    this->buf_ = nullptr;
  }
#endif
}

void UartTcpForwards::load_() {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (this->busy_ || this->loaded_ || worker == nullptr || storage::global_transfer_buffer == nullptr ||
      this->prefs_->path[0] == '\0') {
    return;
  }
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->prefs_->path, &rel);
  if (ps == nullptr) {
    return;  // its storage registers later
  }
  this->buf_ = storage::global_transfer_buffer->try_acquire(FILE_SIZE + 1);
  if (this->buf_ == nullptr) {
    // Another transfer has it
    this->set_timeout("file", RETRY_MS, [this]() { this->load_(); });
    return;
  }
  this->busy_ = true;
  this->buf_len_ = 0;
  const StorageError err = worker->begin_read(ps, rel, &this->handle_, [this](StorageError e) {
    if (e == StorageError::STORAGE_ERROR_OK) {
      this->defer([this]() { this->read_(); });
      return;
    }
    this->busy_ = false;
    this->release_buffer_();
    if (e == StorageError::STORAGE_ERROR_NOT_FOUND) {
      // None yet: made from the current forwards
      this->loaded_ = true;
      if (this->dirty_) {
        this->write_();
      }
    } else {
      ESP_LOGW(TAG, "%s not read: %s", this->prefs_->path, storage::error_to_string(e));
    }
  });
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->busy_ = false;
    this->release_buffer_();
    if (err == StorageError::STORAGE_ERROR_NOT_READY) {
      this->set_timeout("file", RETRY_MS, [this]() { this->load_(); });
    }
  }
#endif
}

void UartTcpForwards::read_() {
  const size_t room = FILE_SIZE - this->buf_len_;
  if (room == 0) {
    this->read_done_(StorageError::STORAGE_ERROR_OK);
    return;
  }
  const StorageError err = storage::global_storage_worker->read_chunk(
      this->handle_, this->buf_ + this->buf_len_, room, &this->got_, [this](StorageError e) {
        if (e != StorageError::STORAGE_ERROR_OK || this->got_ == 0) {
          this->defer([this, e]() { this->read_done_(e); });
          return;
        }
        this->buf_len_ += this->got_;
        this->defer([this]() { this->read_(); });
      });
  if (err == StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->read_(); });
  } else if (err != StorageError::STORAGE_ERROR_OK) {
    this->read_done_(err);
  }
}

void UartTcpForwards::read_done_(StorageError err) {
  const StorageError closed = storage::global_storage_worker->end_read(this->handle_, [this, err](StorageError) {
    this->busy_ = false;
    if (err != StorageError::STORAGE_ERROR_OK) {
      this->release_buffer_();
      ESP_LOGW(TAG, "%s not read: %s", this->prefs_->path, storage::error_to_string(err));
      return;
    }
    this->parse_();
    this->release_buffer_();
    this->loaded_ = true;
    this->change_callback_.call();
    if (this->dirty_) {
      this->write_();
    }
  });
  if (closed == StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this, err]() { this->read_done_(err); });
  }
}

// One line per runner, tab separated: runner, UART (- none), on (1/0), role (server/client), port, host,
// allowed IPs, timeout ms, reconnect ms, baud rate, data bits, parity (N E O M S), stop bits.
void UartTcpForwards::parse_() {
  char *text = reinterpret_cast<char *>(this->buf_);
  text[this->buf_len_] = '\0';
  this->parsing_ = true;
  for (char *line = text; line != nullptr && *line != '\0';) {
    char *next = std::strchr(line, '\n');
    if (next != nullptr) {
      *next++ = '\0';
    }
    char *fields[13] = {};
    size_t count = 0;
    for (char *field = line; field != nullptr && count < 13;) {
      fields[count++] = field;
      field = std::strchr(field, '\t');
      if (field != nullptr) {
        *field++ = '\0';
      }
    }
    line = next;
    if (count < 13) {
      continue;
    }
    const unsigned long index = std::strtoul(fields[0], nullptr, 10);
    if (index >= this->count_) {
      continue;
    }
    UartTcp &runner = this->runners_[index];
    int uart = -1;
    for (size_t u = 0; u < this->uarts_.size(); u++) {
      if (std::strcmp(fields[1], this->uarts_[u].name) == 0) {
        uart = static_cast<int>(u);
      }
    }
    const char *parity = std::strchr(PARITY_CHARS, fields[11][0]);
    this->set_line_settings(index, std::strtoul(fields[9], nullptr, 10),
                            static_cast<uint8_t>(std::strtoul(fields[10], nullptr, 10)),
                            static_cast<uart::UARTParityOptions>(parity != nullptr && *parity != '\0'
                                                                     ? parity - PARITY_CHARS
                                                                     : 0),
                            static_cast<uint8_t>(std::strtoul(fields[12], nullptr, 10)));
    runner.apply_server(std::strcmp(fields[3], "client") != 0);
    runner.apply_port(static_cast<uint16_t>(std::strtoul(fields[4], nullptr, 10)));
    runner.apply_host(fields[5]);
    runner.apply_allowed_ips(fields[6]);
    runner.apply_timeout(std::strtoul(fields[7], nullptr, 10));
    runner.apply_reconnect_interval(std::strtoul(fields[8], nullptr, 10));
    if (!this->set_runner_uart(index, uart)) {
      ESP_LOGW(TAG, "Runner %lu: %s is forwarded by another runner", index, fields[1]);
    }
    runner.set_enabled(fields[2][0] == '1');
  }
  this->parsing_ = false;
}

void UartTcpForwards::write_() {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  if (!this->dirty_ || !this->loaded_ || this->busy_ || storage::global_storage_worker == nullptr ||
      storage::global_transfer_buffer == nullptr || this->prefs_->path[0] == '\0') {
    return;
  }
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->prefs_->path, &rel);
  if (ps == nullptr) {
    return;  // written once its storage is there
  }
  this->buf_ = storage::global_transfer_buffer->try_acquire(FILE_SIZE + 1);
  if (this->buf_ == nullptr) {
    this->set_timeout("file", RETRY_MS, [this]() { this->write_(); });
    return;
  }
  char *out = reinterpret_cast<char *>(this->buf_);
  size_t len = 0;
  // Each piece straight into the buffer; snprintf cuts at its end
  auto put = [&len](int n) { len = std::min(len + static_cast<size_t>(std::max(n, 0)), FILE_SIZE); };
  for (size_t i = 0; i < this->count_ && len < FILE_SIZE; i++) {
    UartTcp &runner = this->runners_[i];
    const Line &line = this->lines_[i];
    put(std::snprintf(out + len, FILE_SIZE - len, "%u\t%s\t%c\t%s\t%u\t%s\t", static_cast<unsigned>(i),
                      line.uart >= 0 ? this->uarts_[line.uart].name : "-", runner.is_enabled() ? '1' : '0',
                      runner.is_server() ? "server" : "client", runner.get_port(), runner.get_host()));
    runner.format_allowed_ips(out + len, FILE_SIZE - len);
    len = std::min(len + std::strlen(out + len), FILE_SIZE);
    put(std::snprintf(out + len, FILE_SIZE - len, "\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%u\t%c\t%u\n",
                      runner.get_timeout(), runner.get_reconnect_interval(), line.baud_rate, line.data_bits,
                      PARITY_CHARS[line.parity], line.stop_bits));
  }
  this->buf_len_ = len;
  this->busy_ = true;
  this->dirty_ = false;
  const StorageError err =
      storage::global_storage_worker->begin_write(ps, rel, &this->handle_, [this](StorageError e) {
        if (e != StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not written: %s", this->prefs_->path, storage::error_to_string(e));
          this->busy_ = false;
          this->dirty_ = true;
          this->release_buffer_();
          return;
        }
        this->defer([this]() { this->write_step_(); });
      });
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->busy_ = false;
    this->dirty_ = true;
    this->release_buffer_();
    if (err == StorageError::STORAGE_ERROR_NOT_READY) {
      this->set_timeout("file", RETRY_MS, [this]() { this->write_(); });
    }
  }
#endif
}

void UartTcpForwards::write_step_() {
  auto close = [this]() {
    const StorageError err = storage::global_storage_worker->end_write(this->handle_, [this](StorageError) {
      this->busy_ = false;
      this->release_buffer_();
      // A new file path came while this ran
      if (!this->loaded_) {
        this->defer([this]() { this->load_(); });
      } else if (this->dirty_) {
        this->defer([this]() { this->write_(); });
      }
    });
    if (err == StorageError::STORAGE_ERROR_NOT_READY) {
      this->defer([this]() { this->write_step_(); });
    }
  };
  if (this->buf_len_ == 0) {
    close();
    return;
  }
  const StorageError err = storage::global_storage_worker->write_chunk(
      this->handle_, this->buf_, this->buf_len_, [this, close](StorageError e) {
        if (e != StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not written: %s", this->prefs_->path, storage::error_to_string(e));
          this->dirty_ = true;
        }
        this->buf_len_ = 0;
        this->defer(close);
      });
  if (err == StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->write_step_(); });
  } else if (err != StorageError::STORAGE_ERROR_OK) {
    this->dirty_ = true;
    this->buf_len_ = 0;
    close();
  }
}

}  // namespace esphome::uart_tcp

#endif  // USE_UART_TCP_FORWARDS
