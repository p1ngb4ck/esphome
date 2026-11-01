#pragma once

#include "esphome/core/defines.h"

#ifdef USE_RUNTIME_LANGUAGE

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/storage/storage.h"
#ifdef USE_RUNTIME_LANGUAGE_LINK
#include "esphome/components/relay_link/relay_link.h"
#endif
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif
#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif

#include <atomic>
#include <string>
#include <vector>

namespace esphome::runtime_language {

static constexpr size_t PATH_MAX_LEN = 128;
static constexpr uint32_t NO_TEXT = UINT32_MAX;

class RuntimeLanguage : public Component {
 public:
  RuntimeLanguage();
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_keys(const char *const *keys, const uint16_t *sorted, uint16_t count);
  void set_max_file_size(uint32_t size) { this->max_file_size_ = size; }
#ifdef USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
#endif
  void set_directory(const char *dir) { this->dir_ = dir; }
  void set_directory(const std::string &dir);
#ifdef USE_RUNTIME_LANGUAGE_LINK
  void set_relay(relay_link::RelayLink *relay) {
    this->link_ = relay;
    this->relay_source_ = true;
  }
  void add_http_source(relay_link::RelayLink *link, const char *base) {
    this->link_ = link;
    if (this->http_count_ < HTTP_SOURCES_MAX)
      this->http_sources_[this->http_count_++] = base;
  }
#endif
  void loop() override;

  const char *english(uint16_t key) const { return key < this->count_ ? this->keys_[key] : ""; }
  const char *text(uint16_t key) const;
  const char *tr(const char *english) const;

  void load(const std::string &path);
  void load_name(const std::string &name);
  void list_fetch();
  const char *get_directory() const { return this->names_ != nullptr ? this->names_->dir : this->dir_; }
  const char *get_name() const { return this->names_ != nullptr ? this->names_->name : ""; }
  bool listed() const { return this->listed_; }
  size_t language_count() const { return this->list_count_; }
  const char *language_name(size_t index) const {
    return index < this->list_count_ && this->list_ != nullptr ? this->list_[index].name : "";
  }
  const char *language_description(size_t index) const {
    return index < this->list_count_ && this->list_ != nullptr ? this->list_[index].description : "";
  }
  void unload();
  void write_template(const std::string &path);
  void apply();

  const char *get_path() const { return this->names_ != nullptr ? this->names_->path : ""; }
  const char *get_error() const { return this->names_ != nullptr ? this->names_->error : ""; }
  bool is_busy() const { return this->busy_ != BUSY_NONE; }
  bool apply_pending() const { return this->next_set_; }
  uint16_t key_count() const { return this->count_; }
  template<typename F> void add_on_change_callback(F &&cb) { this->change_callbacks_.add(std::forward<F>(cb)); }

 protected:
  enum Busy : uint8_t { BUSY_NONE, BUSY_LOAD, BUSY_TEMPLATE };
  static constexpr size_t LIST_MAX = 32;
  static constexpr size_t LIST_BYTES = 4096;
  static constexpr size_t HTTP_SOURCES_MAX = 4;
  static constexpr uint8_t NO_SOURCE = 0xFF;
  struct Entry {
    char name[32];
    char description[48];
  };
  void list_add_(const char *name, const char *description);
  void list_local_();
  void list_done_();
  void remote_load_done_();
#ifdef USE_RUNTIME_LANGUAGE_LINK
  uint8_t next_source_(uint8_t from) const;
  bool source_http_(uint8_t source, const char *path, uint8_t *into, size_t cap, size_t *len);
  static void list_job_(void *ctx, uint16_t index);
  static void get_job_(void *ctx, uint16_t index);
  relay_link::RelayLink *link_{nullptr};
  bool relay_source_{false};
  const char *http_sources_[HTTP_SOURCES_MAX]{};
  uint8_t http_count_{0};
  uint8_t list_source_{NO_SOURCE};
  uint8_t get_source_{NO_SOURCE};
  char *list_buf_{nullptr};
  size_t list_got_{0};
  bool list_ok_{false};
  std::atomic<bool> list_done_flag_{false};
  std::atomic<bool> get_done_flag_{false};
  bool get_ok_{false};
  size_t get_len_{0};
#endif
  const char *dir_{""};
  Entry *list_{nullptr};
  size_t list_count_{0};
  bool listed_{false};
  bool list_busy_{false};
  bool load_now_(const char *path);
  void name_from_path_();
  bool load_name_now_(const char *name);
  void list_now_();
  void unload_now_();
  bool write_template_now_(const char *path);
  struct Names {
    char dir[PATH_MAX_LEN];
    char name[32];
    char path[PATH_MAX_LEN];
    char loading[PATH_MAX_LEN];
    char saved[PATH_MAX_LEN];
    char error[96];
  };
  struct Language {
    char *block{nullptr};
    uint32_t *offsets{nullptr};
  };

  int find_(const char *key) const;
  const char *text_in_(const Language &lang, uint16_t key) const;
  void free_(Language &lang);
  void fail_(const char *why);
  void read_next_();
  void read_done_(storage::StorageError err);
  bool parse_();
  void template_next_();
  void template_done_(storage::StorageError err);
  void save_path_();
  void retry_saved_();

  const char *const *keys_{nullptr};
  const uint16_t *sorted_{nullptr};
  uint16_t count_{0};
  uint32_t max_file_size_{512 * 1024};
#ifdef USE_LVGL
  lvgl::LvglComponent *lvgl_{nullptr};
#endif
  Language shown_{};
  Language next_{};
  bool next_set_{false};
  Busy busy_{BUSY_NONE};
  char *buf_{nullptr};
  size_t buf_len_{0};
  size_t buf_cap_{0};
  size_t got_{0};
  uint16_t template_key_{0};
  bool template_header_{false};
#ifdef USE_STORAGE_WORKER
  storage::StreamHandle handle_{};
#endif
  Names *names_{nullptr};
  ESPPreferenceObject pref_;
  CallbackManager<void()> change_callbacks_;
};

extern RuntimeLanguage *global_runtime_language;

#ifdef USE_LVGL
void bind_label(lv_obj_t *label, uint16_t key);
void bind_label_copy(lv_obj_t *label, uint16_t key);
#endif

inline const char *tr(const char *english) {
  return global_runtime_language != nullptr ? global_runtime_language->tr(english) : english;
}

template<typename... Ts> class LoadAction : public Action<Ts...>, public Parented<RuntimeLanguage> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  TEMPLATABLE_VALUE(std::string, name)
  void set_by_name(bool by_name) { this->by_name_ = by_name; }
  void play(const Ts &...x) override {
    const std::string value = this->by_name_ ? this->name_.value(x...) : this->path_.value(x...);
    if (value.empty()) {
      this->parent_->unload();
    } else if (this->by_name_) {
      this->parent_->load_name(value);
    } else {
      this->parent_->load(value);
    }
  }

 protected:
  bool by_name_{false};
};

template<typename... Ts> class ListAction : public Action<Ts...>, public Parented<RuntimeLanguage> {
 public:
  void play(const Ts &...x) override { this->parent_->list_fetch(); }
};

template<typename... Ts> class UnloadAction : public Action<Ts...>, public Parented<RuntimeLanguage> {
 public:
  void play(const Ts &...x) override { this->parent_->unload(); }
};

template<typename... Ts> class WriteTemplateAction : public Action<Ts...>, public Parented<RuntimeLanguage> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override { this->parent_->write_template(this->path_.value(x...)); }
};

}

#endif
