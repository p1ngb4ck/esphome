#include "remote_desktop.h"

#ifdef USE_REMOTE_DESKTOP

#include "esphome/core/log.h"
#ifdef USE_REMOTE_DESKTOP_USB
#include "select/usb_slot.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace esphome::remote_desktop {

static const char *const TAG = "remote_desktop";

void RemoteDesktop::changed_() {
  this->changed_flag_.store(true);
  this->enable_loop_soon_any_context();
}

void RemoteDesktop::saved_setup_() {
  this->saved_ = static_cast<Saved *>(
      heap_caps_calloc(this->saved_count_, sizeof(Saved), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->saved_usb_ = static_cast<SavedUsb *>(heap_caps_calloc(static_cast<size_t>(this->saved_count_) * SAVED_USB,
                                                              sizeof(SavedUsb), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->saved_ == nullptr || this->saved_usb_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the saved connections");
    heap_caps_free(this->saved_);
    heap_caps_free(this->saved_usb_);
    this->saved_ = nullptr;
    this->saved_usb_ = nullptr;
    return;
  }
#ifdef USE_REMOTE_DESKTOP_CONNECTIONS_FILE
  if (this->file_path_ != nullptr) {
    if (storage::global_storage_registry == nullptr)
      return;
    storage::global_storage_registry->add_on_registered_callback([this](storage::Storage *) {
      if (!this->file_loaded_) {
        this->file_load_();
      } else if (this->file_dirty_) {
        this->file_save_();
      }
    });
    this->file_load_();
    return;
  }
#endif
  this->saved_prefs_ = static_cast<ESPPreferenceObject *>(
      heap_caps_calloc(this->saved_count_, sizeof(ESPPreferenceObject), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->saved_prefs_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the saved connections");
    return;
  }
  const uint32_t key = fnv1_hash("remote_desktop.saved");
  for (uint8_t i = 0; i < this->saved_count_; i++) {
    new (&this->saved_prefs_[i]) ESPPreferenceObject(global_preferences->make_preference<Saved>(key + i));
    Saved &entry = this->saved_[i];
    if (!this->saved_prefs_[i].load(&entry))
      entry = Saved{};
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.host[sizeof(entry.host) - 1] = '\0';
    entry.used = entry.used && entry.host[0] != '\0';
  }
}

bool RemoteDesktop::saved(uint8_t index, std::string &name, std::string &host, uint16_t &port) const {
  if (this->saved_ == nullptr || index >= this->saved_count_ || !this->saved_[index].used)
    return false;
  const Saved &entry = this->saved_[index];
  name = entry.name;
  host = entry.host;
  port = entry.port;
  return true;
}

void RemoteDesktop::save(uint8_t index, const std::string &name, const std::string &host, uint16_t port) {
  if (this->saved_ == nullptr || index >= this->saved_count_) {
    ESP_LOGE(TAG, "No saved connection %u (connections)", index);
    return;
  }
  if (host.empty() || host.size() >= sizeof(Saved::host)) {
    ESP_LOGE(TAG, "The host must be 1 to %u characters", static_cast<unsigned>(sizeof(Saved::host) - 1));
    return;
  }
  Saved &entry = this->saved_[index];
  entry = Saved{};
  std::snprintf(entry.name, sizeof(entry.name), "%s", name.empty() ? host.c_str() : name.c_str());
  std::snprintf(entry.host, sizeof(entry.host), "%s", host.c_str());
  entry.port = port;
  entry.used = true;
  this->saved_changed_(index);
}

void RemoteDesktop::saved_changed_(uint8_t index) {
#ifdef USE_REMOTE_DESKTOP_CONNECTIONS_FILE
  if (this->file_path_ != nullptr) {
    this->file_save_();
    this->change_callbacks_.call();
    return;
  }
#endif
  if (this->saved_prefs_ != nullptr)
    this->saved_prefs_[index].save(&this->saved_[index]);
  this->change_callbacks_.call();
}

int RemoteDesktop::save_new(const std::string &name, const std::string &host, uint16_t port) {
  for (uint8_t i = 0; this->saved_ != nullptr && i < this->saved_count_; i++) {
    if (!this->saved_[i].used) {
      this->save(i, name, host, port);
      return this->saved_[i].used ? i : -1;
    }
  }
  ESP_LOGW(TAG, "All %u saved connections are taken (connections)", this->saved_count_);
  return -1;
}

void RemoteDesktop::forget(uint8_t index) {
  if (this->saved_ == nullptr || index >= this->saved_count_)
    return;
  this->saved_[index] = Saved{};
  std::memset(&this->saved_usb_[index * SAVED_USB], 0, sizeof(SavedUsb) * SAVED_USB);
  if (this->active_saved_ == index)
    this->active_saved_ = -1;
  this->saved_changed_(index);
}

bool RemoteDesktop::connect_saved(uint8_t index, const std::string &password) {
  std::string name, host;
  uint16_t port;
  if (!this->saved(index, name, host, port)) {
    ESP_LOGE(TAG, "No saved connection %u", index);
    return false;
  }
  if (!this->connect(PROTOCOL_AUTO, host, port, password))
    return false;
  this->active_saved_ = static_cast<int8_t>(index);
#ifdef USE_REMOTE_DESKTOP_USB
  this->usb_applying_ = true;
  for (uint8_t i = 0; i < this->usb_count_ && i < SAVED_USB; i++) {
    const SavedUsb &usb = this->saved_usb_[index * SAVED_USB + i];
    this->usb_[i]->want_device(usb.vid, usb.pid, usb.serial);
  }
  this->usb_applying_ = false;
#endif
  return true;
}

#ifdef USE_REMOTE_DESKTOP_USB
void RemoteDesktop::usb_changed(UsbSlot *slot) {
  if (this->usb_applying_ || this->active_saved_ < 0 || this->saved_usb_ == nullptr)
    return;
  for (uint8_t i = 0; i < this->usb_count_ && i < SAVED_USB; i++) {
    if (this->usb_[i] != slot)
      continue;
    SavedUsb &usb = this->saved_usb_[this->active_saved_ * SAVED_USB + i];
    SavedUsb now{};
    slot->selected_device(now.vid, now.pid, now.serial, sizeof(now.serial));
    if (now.vid == usb.vid && now.pid == usb.pid && std::strcmp(now.serial, usb.serial) == 0)
      return;
    usb = now;
    this->saved_changed_(static_cast<uint8_t>(this->active_saved_));
  }
}
#endif

#ifdef USE_REMOTE_DESKTOP_CONNECTIONS_FILE
void RemoteDesktop::file_free_() {
  heap_caps_free(this->file_buf_);
  this->file_buf_ = nullptr;
}

void RemoteDesktop::file_load_() {
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (this->file_busy_ || this->file_loaded_ || worker == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->file_path_, &rel);
  if (ps == nullptr)
    return;
  this->file_buf_ = static_cast<char *>(
      heap_caps_malloc(static_cast<size_t>(this->saved_count_) * FILE_LINE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->file_buf_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM to read %s", this->file_path_);
    return;
  }
  this->file_busy_ = true;
  this->file_len_ = 0;
  const storage::StorageError err = worker->begin_read(ps, rel, &this->file_handle_, [this](storage::StorageError e) {
    if (e == storage::StorageError::STORAGE_ERROR_OK) {
      this->defer([this]() { this->file_read_(); });
      return;
    }
    this->file_busy_ = false;
    this->file_free_();
    if (e == storage::StorageError::STORAGE_ERROR_NOT_FOUND) {
      this->file_loaded_ = true;
      if (this->file_dirty_)
        this->file_save_();
    } else {
      ESP_LOGW(TAG, "%s not read: %s", this->file_path_, storage::error_to_string(e));
    }
  });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->file_busy_ = false;
    this->file_free_();
    if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
      this->set_timeout("connections_file", 500, [this]() { this->file_load_(); });
  }
}

void RemoteDesktop::file_read_() {
  const size_t room = static_cast<size_t>(this->saved_count_) * FILE_LINE - 1 - this->file_len_;
  if (room == 0) {
    this->file_read_done_(storage::StorageError::STORAGE_ERROR_OK);
    return;
  }
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->file_handle_, reinterpret_cast<uint8_t *>(this->file_buf_) + this->file_len_, room, &this->file_got_,
      [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK || this->file_got_ == 0) {
          this->defer([this, e]() { this->file_read_done_(e); });
          return;
        }
        this->file_len_ += this->file_got_;
        this->defer([this]() { this->file_read_(); });
      });
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->file_read_(); });
  } else if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->file_read_done_(err);
  }
}

void RemoteDesktop::file_read_done_(storage::StorageError err) {
  const storage::StorageError closed =
      storage::global_storage_worker->end_read(this->file_handle_, [this, err](storage::StorageError) {
        this->file_busy_ = false;
        if (err != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not read: %s", this->file_path_, storage::error_to_string(err));
          this->file_free_();
          return;
        }
        this->file_parse_();
        this->file_free_();
        this->file_loaded_ = true;
        this->change_callbacks_.call();
        if (this->file_dirty_)
          this->file_save_();
      });
  if (closed == storage::StorageError::STORAGE_ERROR_NOT_READY)
    this->defer([this, err]() { this->file_read_done_(err); });
}

void RemoteDesktop::file_parse_() {
  char *text = this->file_buf_;
  text[this->file_len_] = '\0';
  for (char *line = text; line != nullptr && *line != '\0';) {
    char *next = std::strchr(line, '\n');
    if (next != nullptr)
      *next++ = '\0';
    char *fields[4 + SAVED_USB] = {};
    size_t count = 0;
    for (char *field = line; field != nullptr && count < 4 + SAVED_USB;) {
      fields[count++] = field;
      field = std::strchr(field, '\t');
      if (field != nullptr)
        *field++ = '\0';
    }
    line = next;
    if (count < 4)
      continue;
    const unsigned long index = std::strtoul(fields[0], nullptr, 10);
    if (index >= this->saved_count_ || fields[2][0] == '\0')
      continue;
    Saved &entry = this->saved_[index];
    std::snprintf(entry.name, sizeof(entry.name), "%s", fields[1]);
    std::snprintf(entry.host, sizeof(entry.host), "%s", fields[2]);
    entry.port = static_cast<uint16_t>(std::strtoul(fields[3], nullptr, 10));
    entry.used = true;
    for (size_t i = 0; i < SAVED_USB; i++) {
      SavedUsb &usb = this->saved_usb_[index * SAVED_USB + i];
      usb = SavedUsb{};
      unsigned vid = 0, pid = 0;
      int at = 0;
      if (4 + i < count && std::sscanf(fields[4 + i], "%4x:%4x:%n", &vid, &pid, &at) == 2 && at > 0) {
        usb.vid = static_cast<uint16_t>(vid);
        usb.pid = static_cast<uint16_t>(pid);
        std::snprintf(usb.serial, sizeof(usb.serial), "%s", fields[4 + i] + at);
      }
    }
  }
}

void RemoteDesktop::file_save_() {
  if (this->file_path_ == nullptr || this->saved_ == nullptr)
    return;
  this->file_dirty_ = true;
  if (!this->file_loaded_ || this->file_busy_ || storage::global_storage_worker == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->file_path_, &rel);
  if (ps == nullptr)
    return;
  const size_t cap = static_cast<size_t>(this->saved_count_) * FILE_LINE;
  this->file_buf_ = static_cast<char *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->file_buf_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM to write %s", this->file_path_);
    return;
  }
  auto clean = [](const char *text, char *out, size_t size) {
    std::snprintf(out, size, "%s", text);
    for (char *c = out; *c != '\0'; c++) {
      if (*c == '\t' || *c == '\n' || *c == '\r')
        *c = ' ';
    }
  };
  size_t len = 0;
  for (uint8_t i = 0; i < this->saved_count_; i++) {
    const Saved &entry = this->saved_[i];
    if (!entry.used)
      continue;
    char name[sizeof(entry.name)], host[sizeof(entry.host)];
    clean(entry.name, name, sizeof(name));
    clean(entry.host, host, sizeof(host));
    len += std::snprintf(this->file_buf_ + len, cap - len, "%u\t%s\t%s\t%u", i, name, host, entry.port);
    for (uint8_t u = 0; u < SAVED_USB && len < cap; u++) {
      const SavedUsb &usb = this->saved_usb_[i * SAVED_USB + u];
      char serial[sizeof(usb.serial)];
      clean(usb.serial, serial, sizeof(serial));
      if (usb.vid == 0) {
        len += std::snprintf(this->file_buf_ + len, cap - len, "\t-");
      } else {
        len += std::snprintf(this->file_buf_ + len, cap - len, "\t%04x:%04x:%s", usb.vid, usb.pid, serial);
      }
    }
    if (len < cap)
      len += std::snprintf(this->file_buf_ + len, cap - len, "\n");
    len = std::min(len, cap - 1);
  }
  this->file_len_ = len;
  this->file_busy_ = true;
  this->file_dirty_ = false;
  const storage::StorageError err =
      storage::global_storage_worker->begin_write(ps, rel, &this->file_handle_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not written: %s", this->file_path_, storage::error_to_string(e));
          this->file_busy_ = false;
          this->file_dirty_ = true;
          this->file_free_();
          return;
        }
        this->defer([this]() { this->file_write_(); });
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->file_busy_ = false;
    this->file_dirty_ = true;
    this->file_free_();
    if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
      this->set_timeout("connections_file", 500, [this]() { this->file_save_(); });
  }
}

void RemoteDesktop::file_write_() {
  auto close = [this]() {
    const storage::StorageError err =
        storage::global_storage_worker->end_write(this->file_handle_, [this](storage::StorageError) {
          this->file_busy_ = false;
          this->file_free_();
          if (this->file_dirty_)
            this->defer([this]() { this->file_save_(); });
        });
    if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
      this->defer([this]() { this->file_write_(); });
  };
  if (this->file_len_ == 0) {
    close();
    return;
  }
  const storage::StorageError err = storage::global_storage_worker->write_chunk(
      this->file_handle_, reinterpret_cast<const uint8_t *>(this->file_buf_), this->file_len_,
      [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not written: %s", this->file_path_, storage::error_to_string(e));
          this->file_dirty_ = true;
        }
        this->file_len_ = 0;
        this->defer([this]() { this->file_write_(); });
      });
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->file_write_(); });
  } else if (err != storage::StorageError::STORAGE_ERROR_OK) {
    close();
  }
}
#endif

}

#endif
