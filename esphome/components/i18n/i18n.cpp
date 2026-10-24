#include "i18n.h"

#ifdef USE_I18N

#include "esphome/core/log.h"
#ifdef USE_STORAGE_TRANSFER_BUFFER
#include "esphome/components/storage/transfer_buffer.h"
#endif

#include <cstring>
#include <esp_heap_caps.h>

namespace esphome::i18n {

static const char *const TAG = "i18n";
static constexpr size_t READ_CHUNK = 32 * 1024;
static constexpr size_t TEMPLATE_CHUNK = 16 * 1024;

using storage::StorageError;

I18nComponent *global_i18n = nullptr;

struct SavedPath {
  char path[PATH_MAX_LEN];
};

#ifdef USE_LVGL
struct Binding {
  lv_obj_t *label;
  uint16_t key;
  bool copy;
};

static std::vector<Binding, RAMAllocator<Binding>> &bindings() {
  static std::vector<Binding, RAMAllocator<Binding>> list{RAMAllocator<Binding>(RAMAllocator<Binding>::ALLOC_EXTERNAL)};
  return list;
}

void bind_label(lv_obj_t *label, uint16_t key) { bindings().push_back(Binding{label, key, false}); }

void bind_label_copy(lv_obj_t *label, uint16_t key) { bindings().push_back(Binding{label, key, true}); }

static bool shows(const char *now, const char *text) {
  const size_t n = strlen(now);
  if (n >= 3 && strcmp(now + n - 3, "...") == 0 && strncmp(now, text, n - 3) == 0)
    return true;
  return strcmp(now, text) == 0;
}
#endif

I18nComponent::I18nComponent() { global_i18n = this; }

void I18nComponent::set_keys(const char *const *keys, const uint16_t *sorted, uint16_t count) {
  this->keys_ = keys;
  this->sorted_ = sorted;
  this->count_ = count;
}

void I18nComponent::setup() {
  this->names_ = static_cast<Names *>(heap_caps_calloc(1, sizeof(Names), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->names_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for its state");
    this->mark_failed();
    return;
  }
  this->pref_ = global_preferences->make_preference<SavedPath>(fnv1_hash("i18n_language"));
  SavedPath saved{};
  if (this->pref_.load(&saved)) {
    saved.path[PATH_MAX_LEN - 1] = '\0';
    strncpy(this->names_->saved, saved.path, PATH_MAX_LEN - 1);
  }
  if (this->names_->saved[0] == '\0')
    return;
  if (storage::global_storage_registry != nullptr) {
    storage::global_storage_registry->add_on_registered_callback([this](storage::Storage *) {
      this->defer([this]() { this->retry_saved_(); });
    });
  }
  this->retry_saved_();
}

void I18nComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "i18n:\n"
                "  Texts: %u\n"
                "  Max file size: %u bytes\n"
                "  Language file: %s",
                this->count_, static_cast<unsigned>(this->max_file_size_),
                this->get_path()[0] != '\0' ? this->get_path() : "(English)");
}

void I18nComponent::retry_saved_() {
  if (this->names_ == nullptr || this->names_->saved[0] == '\0' || this->is_busy() ||
      strcmp(this->names_->saved, this->names_->path) == 0)
    return;
  const char *rel = nullptr;
  if (storage::global_storage_registry == nullptr ||
      storage::global_storage_registry->resolve_path(this->names_->saved, &rel) == nullptr)
    return;
  char path[PATH_MAX_LEN];
  strncpy(path, this->names_->saved, PATH_MAX_LEN);
  this->load(path);
}

int I18nComponent::find_(const char *key) const {
  int lo = 0;
  int hi = static_cast<int>(this->count_) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint16_t k = this->sorted_[mid];
    const int c = strcmp(this->keys_[k], key);
    if (c == 0)
      return k;
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return -1;
}

const char *I18nComponent::text_in_(const Language &lang, uint16_t key) const {
  if (lang.block == nullptr || key >= this->count_)
    return this->english(key);
  const uint32_t off = lang.offsets[key];
  if (off == NO_TEXT || lang.block[off] == '\0')
    return this->english(key);
  return lang.block + off;
}

const char *I18nComponent::text(uint16_t key) const {
  return this->text_in_(this->next_set_ ? this->next_ : this->shown_, key);
}

const char *I18nComponent::tr(const char *english) const {
  if (english == nullptr || this->keys_ == nullptr)
    return english;
  const Language &lang = this->next_set_ ? this->next_ : this->shown_;
  if (lang.block == nullptr)
    return english;
  const int key = this->find_(english);
  return key < 0 ? english : this->text_in_(lang, static_cast<uint16_t>(key));
}

void I18nComponent::free_(Language &lang) {
  heap_caps_free(lang.block);
  heap_caps_free(lang.offsets);
  lang = Language{};
}

void I18nComponent::fail_(const char *why) {
  ESP_LOGW(TAG, "%s not loaded: %s", this->names_->loading, why);
  strncpy(this->names_->error, why, sizeof(this->names_->error) - 1);
  this->names_->error[sizeof(this->names_->error) - 1] = '\0';
  heap_caps_free(this->buf_);
  this->buf_ = nullptr;
  this->busy_ = BUSY_NONE;
  this->change_callbacks_.call();
}

bool I18nComponent::load(const char *path) {
#ifdef USE_STORAGE_WORKER
  if (this->names_ == nullptr || this->keys_ == nullptr)
    return false;
  if (this->is_busy()) {
    strncpy(this->names_->error, "busy", sizeof(this->names_->error));
    this->change_callbacks_.call();
    return false;
  }
  strncpy(this->names_->loading, path, PATH_MAX_LEN - 1);
  this->names_->loading[PATH_MAX_LEN - 1] = '\0';
  this->names_->error[0] = '\0';
  this->busy_ = BUSY_LOAD;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                 ? storage::global_storage_registry->resolve_path(this->names_->loading, &rel)
                                 : nullptr;
  if (ps == nullptr || storage::global_storage_worker == nullptr) {
    this->fail_("no storage for this path");
    return false;
  }
  this->buf_ = static_cast<char *>(heap_caps_malloc(this->max_file_size_ + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->buf_ == nullptr) {
    this->fail_("no PSRAM for the file");
    return false;
  }
  this->buf_len_ = 0;
  this->buf_cap_ = this->max_file_size_;
  const StorageError err = storage::global_storage_worker->begin_read(ps, rel, &this->handle_, [this](StorageError e) {
    if (e != StorageError::STORAGE_ERROR_OK) {
      this->fail_(storage::error_to_string(e));
      return;
    }
    this->defer([this]() { this->read_next_(); });
  });
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->fail_(storage::error_to_string(err));
    return false;
  }
  return true;
#else
  return false;
#endif
}

void I18nComponent::read_next_() {
#ifdef USE_STORAGE_WORKER
  const size_t room = this->buf_cap_ - this->buf_len_;
  if (room == 0) {
    this->read_done_(StorageError::STORAGE_ERROR_TRANSFER_TOO_LARGE);
    return;
  }
  const StorageError err = storage::global_storage_worker->read_chunk(
      this->handle_, reinterpret_cast<uint8_t *>(this->buf_ + this->buf_len_), std::min(room, READ_CHUNK), &this->got_,
      [this](StorageError e) {
        if (e != StorageError::STORAGE_ERROR_OK || this->got_ == 0) {
          this->defer([this, e]() { this->read_done_(e); });
          return;
        }
        this->buf_len_ += this->got_;
        this->defer([this]() { this->read_next_(); });
      });
  if (err == StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->read_next_(); });
  } else if (err != StorageError::STORAGE_ERROR_OK) {
    this->read_done_(err);
  }
#endif
}

void I18nComponent::read_done_(StorageError err) {
#ifdef USE_STORAGE_WORKER
  const StorageError closed = storage::global_storage_worker->end_read(this->handle_, [this, err](StorageError) {
    if (err == StorageError::STORAGE_ERROR_TRANSFER_TOO_LARGE) {
      this->fail_("file larger than max_file_size");
      return;
    }
    if (err != StorageError::STORAGE_ERROR_OK) {
      this->fail_(storage::error_to_string(err));
      return;
    }
    if (!this->parse_())
      return;
    strncpy(this->names_->path, this->names_->loading, PATH_MAX_LEN);
    this->save_path_();
    this->busy_ = BUSY_NONE;
    ESP_LOGI(TAG, "Language file %s", this->names_->path);
    this->apply();
    this->change_callbacks_.call();
  });
  if (closed != StorageError::STORAGE_ERROR_OK)
    this->fail_(storage::error_to_string(closed));
#endif
}

static char *unquote(char *p, const char *end) {
  char *w = p;
  while (p < end && *p != '"') {
    if (*p == '\n')
      return nullptr;
    if (*p == '\\' && p + 1 < end) {
      const char c = p[1];
      if (c == 'n') {
        *w++ = '\n';
      } else if (c == 't') {
        *w++ = '\t';
      } else if (c == 'r') {
        *w++ = '\r';
      } else if (c == '"' || c == '\\' || c == '\'') {
        *w++ = c;
      } else {
        *w++ = '\\';
        *w++ = c;
      }
      p += 2;
      continue;
    }
    *w++ = *p++;
  }
  if (p >= end)
    return nullptr;
  *w = '\0';
  return p + 1;
}

static const char *skip_blank(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t'))
    p++;
  return p;
}

bool I18nComponent::parse_() {
  char *const start = this->buf_;
  char *const end = start + this->buf_len_;
  *end = '\0';
  Language lang{};
  lang.offsets = static_cast<uint32_t *>(heap_caps_malloc(sizeof(uint32_t) * this->count_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (lang.offsets == nullptr) {
    this->fail_("no PSRAM for the index");
    return false;
  }
  for (uint16_t k = 0; k < this->count_; k++)
    lang.offsets[k] = NO_TEXT;
  char *p = start;
  if (end - p >= 3 && static_cast<uint8_t>(p[0]) == 0xEF && static_cast<uint8_t>(p[1]) == 0xBB &&
      static_cast<uint8_t>(p[2]) == 0xBF)
    p += 3;
  unsigned line = 0;
  unsigned unknown = 0;
  while (p < end) {
    line++;
    char *eol = static_cast<char *>(memchr(p, '\n', end - p));
    if (eol == nullptr)
      eol = end;
    const char *q = skip_blank(p, eol);
    if (q < eol && *q == '\r' && q + 1 == eol)
      q = eol;
    if (q == eol || *q == '#') {
      p = eol + 1;
      continue;
    }
    if (*q != '"') {
      heap_caps_free(lang.offsets);
      char why[48];
      snprintf(why, sizeof(why), "line %u: a quoted text expected", line);
      this->fail_(why);
      return false;
    }
    char *key = const_cast<char *>(q) + 1;
    char *after = unquote(key, eol);
    const char *v = after != nullptr ? skip_blank(after, eol) : nullptr;
    if (v != nullptr && v < eol && *v == ':')
      v = skip_blank(v + 1, eol);
    char *value = nullptr;
    char *rest = nullptr;
    if (v != nullptr && v < eol && *v == '"') {
      value = const_cast<char *>(v) + 1;
      rest = unquote(value, eol);
    }
    if (rest == nullptr) {
      heap_caps_free(lang.offsets);
      char why[48];
      snprintf(why, sizeof(why), "line %u: \"text\": \"translation\" expected", line);
      this->fail_(why);
      return false;
    }
    const int k = this->find_(key);
    if (k < 0) {
      unknown++;
    } else {
      lang.offsets[k] = static_cast<uint32_t>(value - start);
    }
    p = eol + 1;
  }
  unsigned missing = 0;
  int first = -1;
  for (uint16_t k = 0; k < this->count_; k++) {
    if (lang.offsets[k] == NO_TEXT) {
      missing++;
      if (first < 0)
        first = k;
    }
  }
  if (missing != 0) {
    heap_caps_free(lang.offsets);
    ESP_LOGW(TAG, "%u texts missing, the first: \"%s\"", missing, this->english(static_cast<uint16_t>(first)));
    char why[48];
    snprintf(why, sizeof(why), "%u texts missing", missing);
    this->fail_(why);
    return false;
  }
  if (unknown != 0)
    ESP_LOGI(TAG, "%u texts of the file are not used by this build", unknown);
  char *shrunk = static_cast<char *>(heap_caps_realloc(start, this->buf_len_ + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  lang.block = shrunk != nullptr ? shrunk : start;
  this->buf_ = nullptr;
  if (this->next_set_)
    this->free_(this->next_);
  this->next_ = lang;
  this->next_set_ = true;
  return true;
}

void I18nComponent::unload() {
  if (this->names_ == nullptr || this->busy_ == BUSY_LOAD)
    return;
  if (this->next_set_)
    this->free_(this->next_);
  this->next_ = Language{};
  this->next_set_ = true;
  this->names_->path[0] = '\0';
  this->names_->error[0] = '\0';
  this->save_path_();
  this->apply();
  this->change_callbacks_.call();
}

void I18nComponent::apply() {
  if (!this->next_set_)
    return;
#ifdef USE_LVGL
  if (this->lvgl_ != nullptr && this->lvgl_->is_paused())
    return;
  for (const Binding &b : bindings()) {
    const char *now = lv_label_get_text(b.label);
    const char *old = this->text_in_(this->shown_, b.key);
    if (!b.copy) {
      if (now == old)
        lv_label_set_text_static(b.label, this->text_in_(this->next_, b.key));
    } else if (now != nullptr && shows(now, old)) {
      lv_label_set_text(b.label, this->text_in_(this->next_, b.key));
    }
  }
#endif
  this->free_(this->shown_);
  this->shown_ = this->next_;
  this->next_ = Language{};
  this->next_set_ = false;
}

void I18nComponent::save_path_() {
  SavedPath saved{};
  strncpy(saved.path, this->names_->path, PATH_MAX_LEN - 1);
  strncpy(this->names_->saved, this->names_->path, PATH_MAX_LEN);
  this->pref_.save(&saved);
}

static size_t escaped(char *out, const char *text) {
  size_t n = 0;
  for (const char *p = text; *p != '\0'; p++) {
    const char c = *p;
    if (c == '"' || c == '\\') {
      out[n++] = '\\';
      out[n++] = c;
    } else if (c == '\n') {
      out[n++] = '\\';
      out[n++] = 'n';
    } else if (c == '\t') {
      out[n++] = '\\';
      out[n++] = 't';
    } else if (c == '\r') {
      out[n++] = '\\';
      out[n++] = 'r';
    } else {
      out[n++] = c;
    }
  }
  return n;
}

bool I18nComponent::write_template(const char *path) {
#if defined(USE_STORAGE_WORKER) && defined(USE_STORAGE_TRANSFER_BUFFER)
  if (this->names_ == nullptr || this->keys_ == nullptr || this->is_busy())
    return false;
  strncpy(this->names_->loading, path, PATH_MAX_LEN - 1);
  this->names_->loading[PATH_MAX_LEN - 1] = '\0';
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                 ? storage::global_storage_registry->resolve_path(this->names_->loading, &rel)
                                 : nullptr;
  if (ps == nullptr || storage::global_storage_worker == nullptr || storage::global_transfer_buffer == nullptr)
    return false;
  uint8_t *buf = storage::global_transfer_buffer->try_acquire(TEMPLATE_CHUNK);
  if (buf == nullptr) {
    strncpy(this->names_->error, "transfer buffer busy", sizeof(this->names_->error));
    this->change_callbacks_.call();
    return false;
  }
  this->buf_ = reinterpret_cast<char *>(buf);
  this->buf_cap_ = TEMPLATE_CHUNK;
  this->template_key_ = 0;
  this->busy_ = BUSY_TEMPLATE;
  const StorageError err = storage::global_storage_worker->begin_write(ps, rel, &this->handle_, [this](StorageError e) {
    if (e != StorageError::STORAGE_ERROR_OK) {
      storage::global_transfer_buffer->release();
      this->buf_ = nullptr;
      this->busy_ = BUSY_NONE;
      strncpy(this->names_->error, storage::error_to_string(e), sizeof(this->names_->error) - 1);
      this->change_callbacks_.call();
      return;
    }
    this->defer([this]() { this->template_next_(); });
  });
  if (err != StorageError::STORAGE_ERROR_OK) {
    storage::global_transfer_buffer->release();
    this->buf_ = nullptr;
    this->busy_ = BUSY_NONE;
    return false;
  }
  return true;
#else
  return false;
#endif
}

void I18nComponent::template_next_() {
#if defined(USE_STORAGE_WORKER) && defined(USE_STORAGE_TRANSFER_BUFFER)
  size_t n = 0;
  while (this->template_key_ < this->count_) {
    const char *key = this->english(this->template_key_);
    const char *now = this->text(this->template_key_);
    const char *value = now == key ? "" : now;
    const size_t need = 2 * (strlen(key) + strlen(value)) + 8;
    if (need > this->buf_cap_) {
      this->template_key_++;
      continue;
    }
    if (n + need > this->buf_cap_)
      break;
    this->buf_[n++] = '"';
    n += escaped(this->buf_ + n, key);
    memcpy(this->buf_ + n, "\": \"", 4);
    n += 4;
    n += escaped(this->buf_ + n, value);
    this->buf_[n++] = '"';
    this->buf_[n++] = '\n';
    this->template_key_++;
  }
  if (n == 0) {
    this->template_done_(StorageError::STORAGE_ERROR_OK);
    return;
  }
  const StorageError err = storage::global_storage_worker->write_chunk(
      this->handle_, reinterpret_cast<const uint8_t *>(this->buf_), n, [this](StorageError e) {
        if (e != StorageError::STORAGE_ERROR_OK) {
          this->defer([this, e]() { this->template_done_(e); });
          return;
        }
        this->defer([this]() { this->template_next_(); });
      });
  if (err != StorageError::STORAGE_ERROR_OK)
    this->template_done_(err);
#endif
}

void I18nComponent::template_done_(StorageError err) {
#if defined(USE_STORAGE_WORKER) && defined(USE_STORAGE_TRANSFER_BUFFER)
  const StorageError closed = storage::global_storage_worker->end_write(this->handle_, [this, err](StorageError e) {
    storage::global_transfer_buffer->release();
    this->buf_ = nullptr;
    this->busy_ = BUSY_NONE;
    const StorageError result = err != StorageError::STORAGE_ERROR_OK ? err : e;
    if (result != StorageError::STORAGE_ERROR_OK) {
      strncpy(this->names_->error, storage::error_to_string(result), sizeof(this->names_->error) - 1);
      ESP_LOGW(TAG, "Template %s not written: %s", this->names_->loading, storage::error_to_string(result));
    } else {
      this->names_->error[0] = '\0';
      ESP_LOGI(TAG, "Template %s written (%u texts)", this->names_->loading, this->count_);
    }
    this->change_callbacks_.call();
  });
  if (closed != StorageError::STORAGE_ERROR_OK) {
    storage::global_transfer_buffer->release();
    this->buf_ = nullptr;
    this->busy_ = BUSY_NONE;
    this->change_callbacks_.call();
  }
#endif
}

}

#endif
