#include "bt_persistence.h"

#ifdef USE_BT_PERSISTENCE

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <osi/esphome_nvs_store.h>

#include <cinttypes>
#include <cstring>

namespace esphome::bt_persistence {

using storage::StorageError;

static const char *const TAG = "bt_persistence";
static constexpr size_t READ_CHUNK = 16384;
static constexpr size_t MAX_IMAGE = 4 * 1024 * 1024;

BtPersistence *global_bt_persistence = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void BtPersistence::setup() {
  global_bt_persistence = this;
  if (!esphome_nvs_store_init()) {
    this->fail_("no lock for the store");
    return;
  }
  this->path_ = this->default_path_;
  const uint32_t default_hash = fnv1_hash(this->default_path_);
  this->pref_ = global_preferences->make_preference<PathPref>(fnv1_hash("bt_persistence_path"), true);
  PathPref saved{};
  if (this->pref_.load(&saved) && saved.default_hash == default_hash && saved.path[0] == '/') {
    saved.path[sizeof(saved.path) - 1] = '\0';
    this->path_ = saved.path;
  }
}

void BtPersistence::dump_config() {
  static const char *const STATES[] = {"waiting for the storage", "loading", "ready", "failed"};
  const State state = this->state_.load(std::memory_order_acquire);
  ESP_LOGCONFIG(TAG,
                "Bluetooth persistence:\n"
                "  File: %s\n"
                "  Write delay: %" PRIu32 " ms\n"
                "  Storage timeout: %" PRIu32 " ms\n"
                "  State: %s%s%s",
                this->path_.c_str(), this->write_delay_, this->storage_timeout_,
                STATES[static_cast<uint8_t>(state)], state == State::FAILED ? ": " : "",
                state == State::FAILED ? this->fail_reason_ : "");
}

bool BtPersistence::resolve_() {
  const char *rel = nullptr;
  storage::PathStorage *s = storage::global_storage_registry->resolve_path(this->path_.c_str(), &rel);
  if (s == nullptr || rel == nullptr || rel[0] != '/' || rel[1] == '\0')
    return false;
  this->storage_ = s;
  this->rel_ = rel;
  this->tmp_ = this->rel_ + ".tmp";
  return true;
}

bool BtPersistence::storage_busy_() const { return storage::global_storage_worker->has_active_task_io(this->storage_); }

void BtPersistence::loop() {
  const uint32_t now = millis();
  const State state = this->state_.load(std::memory_order_relaxed);

  if (this->path_pending_ && !this->writing_ && state != State::LOADING) {
    this->apply_pending_path_();
    return;
  }

  if (state == State::WAITING) {
    // The first loop() runs only once every component's setup() is done: the wait counts from
    // there, and a storage that is there is taken however long the boot took.
    if (this->last_try_ == 0)
      this->wait_start_ = now;
    else if (now - this->last_try_ < 250)
      return;
    this->last_try_ = now;
    if (this->resolve_()) {
      this->state_.store(State::LOADING, std::memory_order_release);
      this->recovered_ = false;
      this->start_open_();
    } else if (now - this->wait_start_ > this->storage_timeout_) {
      this->fail_("the storage holding the file is not available");
    }
    return;
  }
  if (state != State::READY || this->writing_)
    return;

  const uint32_t gen = esphome_nvs_store_generation();
  if (gen != this->seen_gen_) {
    this->seen_gen_ = gen;
    this->changed_ms_ = now;
  }
  if (gen != this->saved_gen_ && now - this->changed_ms_ >= this->write_delay_)
    this->start_write_();
}

// -- Loading -------------------------------------------------------------------------------

void BtPersistence::start_open_() {
  this->in_len_ = 0;
  StorageError err = storage::global_storage_worker->begin_read(
      this->storage_, this->rel_.c_str(), &this->handle_, [this](StorageError result) {
        if (result == StorageError::STORAGE_ERROR_OK) {
          this->read_next_();
        } else if (result == StorageError::STORAGE_ERROR_NOT_FOUND && !this->recovered_) {
          this->recover_();
        } else if (result == StorageError::STORAGE_ERROR_NOT_READY) {
          this->state_.store(State::WAITING, std::memory_order_release);
        } else {
          ESP_LOGE(TAG, "Opening %s: %s", this->path_.c_str(), storage::error_to_string(result));
          this->fail_("the file cannot be read");
        }
      });
  if (err == StorageError::STORAGE_ERROR_NOT_READY) {
    // Every stream in use (others read at boot too): tried again from loop().
    this->state_.store(State::WAITING, std::memory_order_release);
  } else if (err != StorageError::STORAGE_ERROR_OK) {
    ESP_LOGE(TAG, "Opening %s: %s", this->path_.c_str(), storage::error_to_string(err));
    this->fail_("the storage worker refused the read");
  }
}

void BtPersistence::read_next_() {
  if (!this->grow_(this->in_buf_, this->in_cap_, this->in_len_ + READ_CHUNK)) {
    storage::global_storage_worker->end_read(this->handle_, [](StorageError) {});
    this->fail_("the file is too large or PSRAM is short");
    return;
  }
  StorageError err = storage::global_storage_worker->read_chunk(
      this->handle_, this->in_buf_ + this->in_len_, READ_CHUNK, &this->got_, [this](StorageError result) {
        if (result != StorageError::STORAGE_ERROR_OK) {
          storage::global_storage_worker->end_read(this->handle_, [](StorageError) {});
          if (result == StorageError::STORAGE_ERROR_NOT_FOUND && this->in_len_ == 0 && !this->recovered_) {
            this->recover_();
          } else if (result == StorageError::STORAGE_ERROR_NOT_READY) {
            this->state_.store(State::WAITING, std::memory_order_release);
          } else {
            ESP_LOGE(TAG, "Reading %s: %s", this->path_.c_str(), storage::error_to_string(result));
            this->fail_("the file cannot be read");
          }
          return;
        }
        if (this->got_ == 0) {
          storage::global_storage_worker->end_read(this->handle_, [this](StorageError) { this->finish_read_(); });
          return;
        }
        this->in_len_ += this->got_;
        this->read_next_();
      });
  if (err != StorageError::STORAGE_ERROR_OK) {
    storage::global_storage_worker->end_read(this->handle_, [](StorageError) {});
    this->fail_("the storage worker refused the read");
  }
}

void BtPersistence::finish_read_() {
  const bool ok = esphome_nvs_store_load(this->in_buf_, this->in_len_);
  heap_caps_free(this->in_buf_);
  this->in_buf_ = nullptr;
  this->in_cap_ = 0;
  if (!ok) {
    this->fail_("the file is damaged");
    return;
  }
  ESP_LOGV(TAG, "Loaded %u bytes from %s", static_cast<unsigned>(this->in_len_), this->path_.c_str());
  this->set_ready_();
}

// A save replaces the file by removing it and renaming the temporary one; power lost in between
// leaves only the temporary one, complete.
void BtPersistence::recover_() {
  this->recovered_ = true;
  StorageError err = storage::global_storage_worker->async_move(
      this->storage_, this->tmp_.c_str(), this->storage_, this->rel_.c_str(),
      [this](StorageError result) {
        if (result == StorageError::STORAGE_ERROR_OK) {
          ESP_LOGV(TAG, "Recovered %s from an interrupted save", this->path_.c_str());
          this->start_open_();
        } else if (result == StorageError::STORAGE_ERROR_NOT_FOUND) {
          this->create_();
        } else {
          ESP_LOGE(TAG, "Recovering %s: %s", this->path_.c_str(), storage::error_to_string(result));
          this->fail_("the interrupted save cannot be recovered");
        }
      },
      nullptr, true);
  if (err != StorageError::STORAGE_ERROR_OK)
    this->fail_("the storage worker refused the recovery");
}

bool BtPersistence::make_parents_() {
  for (size_t pos = this->rel_.find('/', 1); pos != std::string::npos; pos = this->rel_.find('/', pos + 1)) {
    const std::string dir = this->rel_.substr(0, pos);
    StorageError err = this->storage_->mkdir(dir.c_str());
    if (err != StorageError::STORAGE_ERROR_OK && err != StorageError::STORAGE_ERROR_ALREADY_EXISTS) {
      ESP_LOGE(TAG, "Creating %s: %s", dir.c_str(), storage::error_to_string(err));
      return false;
    }
  }
  return true;
}

void BtPersistence::create_() {
  if (this->storage_busy_()) {
    this->set_timeout("create", 50, [this]() { this->create_(); });
    return;
  }
  if (!this->make_parents_()) {
    this->fail_("the folder for the file cannot be created");
    return;
  }
  StorageError err = storage::global_storage_worker->begin_write(
      this->storage_, this->rel_.c_str(), &this->handle_, [this](StorageError result) {
        if (result != StorageError::STORAGE_ERROR_OK) {
          ESP_LOGE(TAG, "Creating %s: %s", this->path_.c_str(), storage::error_to_string(result));
          this->fail_("the file cannot be created");
          return;
        }
        storage::global_storage_worker->end_write(this->handle_, [this](StorageError closed) {
          if (closed != StorageError::STORAGE_ERROR_OK || !esphome_nvs_store_load(nullptr, 0)) {
            this->fail_("the file cannot be created");
            return;
          }
          ESP_LOGV(TAG, "Created %s", this->path_.c_str());
          this->set_ready_();
        });
      });
  if (err == StorageError::STORAGE_ERROR_NOT_READY) {
    this->state_.store(State::WAITING, std::memory_order_release);
  } else if (err != StorageError::STORAGE_ERROR_OK) {
    this->fail_("the storage worker refused the write");
  }
}

void BtPersistence::set_ready_() {
  this->seen_gen_ = this->saved_gen_ = esphome_nvs_store_generation();
  this->state_.store(State::READY, std::memory_order_release);
  auto waiting = std::move(this->waiting_);
  this->waiting_.clear();
  for (auto &cb : waiting)
    cb();
  for (auto &cb : this->ready_callbacks_)
    cb();
}

void BtPersistence::fail_(const char *why) {
  ESP_LOGE(TAG, "Bluetooth stays off: %s (%s)", why, this->path_.c_str());
  this->fail_reason_ = why;
  this->state_.store(State::FAILED, std::memory_order_release);
  heap_caps_free(this->in_buf_);
  this->in_buf_ = nullptr;
  this->in_cap_ = 0;
  this->status_set_error(LOG_STR("Bluetooth data not available"));
  for (auto &cb : this->error_callbacks_)
    cb();
}

void BtPersistence::when_ready(std::function<void()> &&cb) {
  if (this->is_ready()) {
    cb();
  } else {
    this->waiting_.push_back(std::move(cb));
  }
}

// -- Saving --------------------------------------------------------------------------------

void BtPersistence::start_write_() {
  if (!this->resolve_()) {
    this->changed_ms_ = millis();
    return;
  }
  size_t need = 0;
  while (!esphome_nvs_store_serialize(this->out_buf_, this->out_cap_, &need, &this->write_gen_)) {
    if (need == 0 || !this->grow_(this->out_buf_, this->out_cap_, need)) {
      ESP_LOGW(TAG, "No PSRAM to save the Bluetooth data");
      this->changed_ms_ = millis();
      return;
    }
  }
  this->out_len_ = need;
  this->writing_ = true;
  StorageError err = storage::global_storage_worker->begin_write(
      this->storage_, this->tmp_.c_str(), &this->handle_, [this](StorageError opened) {
        if (opened != StorageError::STORAGE_ERROR_OK) {
          this->write_failed_(opened, false);
          return;
        }
        StorageError queued = storage::global_storage_worker->write_chunk(
            this->handle_, this->out_buf_, this->out_len_, [this](StorageError written) {
              if (written != StorageError::STORAGE_ERROR_OK) {
                this->write_failed_(written, true);
                return;
              }
              storage::global_storage_worker->end_write(this->handle_, [this](StorageError closed) {
                if (closed != StorageError::STORAGE_ERROR_OK) {
                  this->write_failed_(closed, false);
                  return;
                }
                StorageError moved = storage::global_storage_worker->async_move(
                    this->storage_, this->tmp_.c_str(), this->storage_, this->rel_.c_str(),
                    [this](StorageError result) {
                      if (result != StorageError::STORAGE_ERROR_OK) {
                        this->write_failed_(result, false);
                        return;
                      }
                      this->saved_gen_ = this->write_gen_;
                      this->writing_ = false;
                      ESP_LOGV(TAG, "Saved %u bytes to %s", static_cast<unsigned>(this->out_len_), this->path_.c_str());
                    },
                    nullptr, true);
                if (moved != StorageError::STORAGE_ERROR_OK)
                  this->write_failed_(moved, false);
              });
            });
        if (queued != StorageError::STORAGE_ERROR_OK)
          this->write_failed_(queued, true);
      });
  if (err != StorageError::STORAGE_ERROR_OK)
    this->write_failed_(err, false);
}

void BtPersistence::write_failed_(StorageError err, bool close) {
  ESP_LOGW(TAG, "Saving to %s failed (%s); trying again", this->path_.c_str(), storage::error_to_string(err));
  if (close)
    storage::global_storage_worker->end_write(this->handle_, [](StorageError) {});
  this->writing_ = false;
  this->changed_ms_ = millis();
}

void BtPersistence::on_shutdown() {
  if (this->state_.load(std::memory_order_acquire) != State::READY || this->writing_ ||
      esphome_nvs_store_generation() == this->saved_gen_ || !this->resolve_() || this->storage_busy_())
    return;
  size_t need = 0;
  while (!esphome_nvs_store_serialize(this->out_buf_, this->out_cap_, &need, &this->write_gen_)) {
    if (need == 0 || !this->grow_(this->out_buf_, this->out_cap_, need))
      return;
  }
  if (storage::write_file(this->storage_, this->tmp_.c_str(), this->out_buf_, need) != StorageError::STORAGE_ERROR_OK)
    return;
  this->storage_->remove(this->rel_.c_str());
  if (this->storage_->rename(this->tmp_.c_str(), this->rel_.c_str()) == StorageError::STORAGE_ERROR_OK)
    this->saved_gen_ = this->write_gen_;
}

// -- Path ----------------------------------------------------------------------------------

void BtPersistence::change_path(const std::string &path) {
  if (path.empty() || path[0] != '/' || path.size() >= sizeof(PathPref::path)) {
    ESP_LOGW(TAG, "Not a usable path: '%s'", path.c_str());
    return;
  }
  this->pending_path_ = path;
  this->path_pending_ = true;
}

void BtPersistence::apply_pending_path_() {
  this->path_pending_ = false;
  if (this->pending_path_ == this->path_)
    return;
  this->path_ = this->pending_path_;
  PathPref pref{};
  pref.default_hash = fnv1_hash(this->default_path_);
  std::strncpy(pref.path, this->path_.c_str(), sizeof(pref.path) - 1);
  this->pref_.save(&pref);
  global_preferences->sync();

  if (this->state_.load(std::memory_order_acquire) == State::READY) {
    ESP_LOGV(TAG, "Moving the Bluetooth data to %s", this->path_.c_str());
    this->saved_gen_ = esphome_nvs_store_generation() + 1;
    this->changed_ms_ = millis() - this->write_delay_;
    return;
  }
  ESP_LOGV(TAG, "Loading the Bluetooth data from %s", this->path_.c_str());
  this->status_clear_error();
  this->state_.store(State::WAITING, std::memory_order_release);
  this->wait_start_ = millis();
}

bool BtPersistence::grow_(uint8_t *&buf, size_t &cap, size_t need) {
  if (need <= cap)
    return true;
  if (need > MAX_IMAGE)
    return false;
  size_t size = cap == 0 ? 32768 : cap;
  while (size < need)
    size *= 2;
  auto *fresh = static_cast<uint8_t *>(heap_caps_realloc(buf, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (fresh == nullptr)
    return false;
  buf = fresh;
  cap = size;
  return true;
}

}  // namespace esphome::bt_persistence

#endif  // USE_BT_PERSISTENCE
