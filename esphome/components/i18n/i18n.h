#pragma once

#include "esphome/core/defines.h"

#ifdef USE_I18N

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/storage/storage.h"
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif
#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif

#include <string>
#include <vector>

namespace esphome::i18n {

static constexpr size_t PATH_MAX_LEN = 128;
static constexpr uint32_t NO_TEXT = UINT32_MAX;

class I18nComponent : public Component {
 public:
  I18nComponent();
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_keys(const char *const *keys, const uint16_t *sorted, uint16_t count);
  void set_max_file_size(uint32_t size) { this->max_file_size_ = size; }
#ifdef USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
#endif

  const char *english(uint16_t key) const { return key < this->count_ ? this->keys_[key] : ""; }
  const char *text(uint16_t key) const;
  const char *tr(const char *english) const;

  bool load(const char *path);
  void unload();
  bool write_template(const char *path);
  void apply();

  const char *get_path() const { return this->names_ != nullptr ? this->names_->path : ""; }
  const char *get_error() const { return this->names_ != nullptr ? this->names_->error : ""; }
  bool is_busy() const { return this->busy_ != BUSY_NONE; }
  bool apply_pending() const { return this->next_set_; }
  uint16_t key_count() const { return this->count_; }
  template<typename F> void add_on_change_callback(F &&cb) { this->change_callbacks_.add(std::forward<F>(cb)); }

 protected:
  enum Busy : uint8_t { BUSY_NONE, BUSY_LOAD, BUSY_TEMPLATE };
  struct Names {
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

extern I18nComponent *global_i18n;

#ifdef USE_LVGL
void bind_label(lv_obj_t *label, uint16_t key);
#endif

inline const char *tr(const char *english) {
  return global_i18n != nullptr ? global_i18n->tr(english) : english;
}

template<typename... Ts> class LoadAction : public Action<Ts...>, public Parented<I18nComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override {
    const std::string path = this->path_.value(x...);
    if (path.empty()) {
      this->parent_->unload();
    } else {
      this->parent_->load(path.c_str());
    }
  }
};

template<typename... Ts> class UnloadAction : public Action<Ts...>, public Parented<I18nComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->unload(); }
};

template<typename... Ts> class WriteTemplateAction : public Action<Ts...>, public Parented<I18nComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override { this->parent_->write_template(this->path_.value(x...).c_str()); }
};

}

#endif
