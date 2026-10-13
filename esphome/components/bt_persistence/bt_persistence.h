#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BT_PERSISTENCE

#include "esphome/components/storage/storage_worker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace esphome::bt_persistence {

// Keeps Bluedroid's pairing data (and GATT cache) in PSRAM and mirrors it to a file through the
// storage worker. Bluetooth is held back until the file is loaded.
class BtPersistence : public Component {
 public:
  enum class State : uint8_t { WAITING, LOADING, READY, FAILED };

  void set_default_path(const char *path) { this->default_path_ = path; }
  void set_write_delay(uint32_t ms) { this->write_delay_ = ms; }
  void set_storage_timeout(uint32_t ms) { this->storage_timeout_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  // Any task.
  bool is_ready() const { return this->state_.load(std::memory_order_acquire) == State::READY; }
  bool has_failed() const { return this->state_.load(std::memory_order_acquire) == State::FAILED; }

  // Main loop. Runs cb at once when loaded, otherwise once loading succeeds.
  void when_ready(std::function<void()> &&cb);
  // Main loop. Moves the stored data to path; when nothing is loaded yet, loads from there.
  void change_path(const std::string &path);
  const std::string &get_path() const { return this->path_; }

  void add_on_ready_callback(std::function<void()> &&cb) { this->ready_callbacks_.push_back(std::move(cb)); }
  void add_on_error_callback(std::function<void()> &&cb) { this->error_callbacks_.push_back(std::move(cb)); }

 protected:
  struct PathPref {
    uint32_t default_hash;
    char path[192];
  };

  bool resolve_();
  bool storage_busy_() const;
  void start_open_();
  void read_next_();
  void finish_read_();
  void recover_();
  void create_();
  bool make_parents_();
  void set_ready_();
  void fail_(const char *why);
  void start_write_();
  void write_failed_(storage::StorageError err, bool close);
  void apply_pending_path_();
  bool grow_(uint8_t *&buf, size_t &cap, size_t need);

  const char *default_path_{""};
  std::string path_;
  std::string pending_path_;
  bool path_pending_{false};
  storage::PathStorage *storage_{nullptr};
  std::string rel_;
  std::string tmp_;
  ESPPreferenceObject pref_;

  std::atomic<State> state_{State::WAITING};
  uint32_t wait_start_{0};
  uint32_t last_try_{0};
  uint32_t storage_timeout_{20000};
  bool recovered_{false};

  storage::StreamHandle handle_{};
  uint8_t *in_buf_{nullptr};
  size_t in_cap_{0};
  size_t in_len_{0};
  size_t got_{0};

  uint8_t *out_buf_{nullptr};
  size_t out_cap_{0};
  size_t out_len_{0};
  bool writing_{false};
  uint32_t write_delay_{2000};
  uint32_t seen_gen_{0};
  uint32_t saved_gen_{0};
  uint32_t write_gen_{0};
  uint32_t changed_ms_{0};

  std::vector<std::function<void()>> waiting_;
  std::vector<std::function<void()>> ready_callbacks_;
  std::vector<std::function<void()>> error_callbacks_;
};

extern BtPersistence *global_bt_persistence;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

class ReadyTrigger : public Trigger<> {
 public:
  explicit ReadyTrigger(BtPersistence *parent) {
    parent->add_on_ready_callback([this]() { this->trigger(); });
  }
};

class ErrorTrigger : public Trigger<> {
 public:
  explicit ErrorTrigger(BtPersistence *parent) {
    parent->add_on_error_callback([this]() { this->trigger(); });
  }
};

template<typename... Ts> class SetPathAction : public Action<Ts...>, public Parented<BtPersistence> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override { this->parent_->change_path(this->path_.value(x...)); }
};

}  // namespace esphome::bt_persistence

#endif  // USE_BT_PERSISTENCE
