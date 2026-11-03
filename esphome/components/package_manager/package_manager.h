#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include "module_abi.h"
#include "package_loader.h"
#include "package_store.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/sha256/sha256.h"
#include "ipk_reader.h"
#ifdef USE_PACKAGE_MANAGER_HTTP
#include "esphome/components/http_request/http_request.h"
#endif
#ifdef USE_JSON
#include "esphome/components/json/json_util.h"
#endif
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome::package_manager {

class PackageManager;

enum class AfterStop : uint8_t { NONE, DISABLE, REMOVE, REPLACE };

static const uint8_t RECOVERY_BIT = 0x01;
static const uint8_t HOLD_BIT = 0x02;

struct PackageRegistration {
  RegistrationUndo undo;
  void *target;
  void *first;
  void *second;
};

struct Package : StoreEntry {
  LoadedPackage module;
  module_loop_fn loop;
  module_stop_fn stop;
  module_deinit_fn deinit;
  PackageRegistration *registrations;
  uint16_t registration_count;
  uint16_t registration_capacity;
  module_host_api_t api;
  PackageManager *manager;
  AfterStop after;
  bool stopping;
};

struct PackageUnpack {
  IpkReader ipk;
  sha256::SHA256 sha;
  storage::StreamHandle in_stream{};
  StoreEntry entry{};
  size_t read_len{0};
  size_t control_len{0};
  size_t sums_len{0};
  bool read_end{false};
  bool data_open{false};
  bool data_done{false};
  uint8_t line_kind{0};
  size_t line_len{0};
  uint32_t missing{0};
  uint32_t written{0};
  const uint8_t *chunk{nullptr};
  size_t chunk_left{0};
  char line[512];
  char control[2048];
  char sums[4096];
  uint8_t read_buf[32 * 1024];
};

#ifdef USE_PACKAGE_MANAGER_HTTP
static const size_t REPO_MAX = 8;
static const size_t AVAILABLE_STEP = 16;
static const size_t REPO_QUEUE_MAX = STORE_MAX_ENTRIES;
static const size_t INDEX_IN_SIZE = 1536;
static const size_t INDEX_LINE_SIZE = 512;

enum class RepoState : uint8_t { NONE, OK, FAILED };

struct Repository {
  char name[24];
  char url[128];
  uint8_t key[40];
  uint8_t has_key;
  uint8_t signed_ok;
  uint8_t used;
  uint8_t enabled;
  uint8_t builtin;
  RepoState state;
  uint16_t count;
};

struct IndexInflate;
struct IndexSignature;

struct IndexReader {
  uint8_t in[INDEX_IN_SIZE];
  char line[INDEX_LINE_SIZE];
  size_t in_len;
  size_t line_len;
  IndexInflate *inflate;
  IndexSignature *signature;
  uint8_t gz_state;
  bool arch_ok;
  bool reading_signature;
  bool checking;
  uint8_t fetching;
};

struct AvailablePackage {
  char name[sizeof(StoreEntry::name)];
  char version[sizeof(StoreEntry::version)];
  char depends[sizeof(StoreEntry::depends)];
  char filename[64];
  char description[64];
  uint8_t sha256[32];
  uint32_t size;
  uint32_t flash;
  uint32_t psram;
  uint32_t abi;
  uint8_t repo;
  uint8_t has_sha256;
};

struct PackageDownload {
  sha256::SHA256 sha;
  uint8_t expected[32]{};
  bool check{false};
  uint8_t chunk[32 * 1024];
};
#endif

class PackageManager : public Component {
 public:
  void set_directory(const char *directory) { this->directory_ = directory; }
  void set_max_packages(uint8_t count) { this->max_packages_ = count; }
#ifdef USE_PACKAGE_MANAGER_HTTP
  void set_http_request_parent(http_request::HttpRequestComponent *http) { this->http_ = http; }
#endif
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  bool load();
  bool install(const std::string &source, const std::string &name, const std::string &version, uint32_t abi,
               SettingsTarget settings, const std::string &target);
  bool set_enabled(const std::string &name, bool enabled);
  bool set_recovery(const std::string &name, bool recovery);
  bool set_hold(const std::string &name, bool hold);
  bool remove(const std::string &name);
  uint32_t firmware_id() const;
  const char *last_error() const { return this->last_error_; }
  bool is_installing() const { return this->installing_; }
  const char *installing_name() const { return this->installing_ ? this->pending_name_ : ""; }
  template<typename F> void add_on_change_callback(F &&callback) { this->on_change_.add(std::forward<F>(callback)); }
#ifdef USE_JSON
  void dump_installed(JsonObject root);
  void dump_info(const std::string &name, JsonObject root);
  void dump_files(const std::string &name, JsonObject root);
  void dump_find(const std::string &query, JsonObject root);
  void dump_depends(const std::string &name, bool reverse, bool recursive, JsonObject root);
  void dump_architecture(JsonObject root);
#ifdef USE_PACKAGE_MANAGER_HTTP
  void dump_available(JsonObject root);
#endif
#endif
#ifdef USE_PACKAGE_MANAGER_API
  void register_api_actions();
#endif
#ifdef USE_PACKAGE_MANAGER_HTTP
  void add_repository(const char *name, const char *url, const char *public_key_hex);
  bool refresh();
  bool install_from_repo(const std::string &name);
  bool download(const std::string &name, const std::string &folder);
  bool repo_add(const std::string &name, const std::string &url, const std::string &public_key);
  bool repo_delete(const std::string &name);
  bool repo_enable(const std::string &name, bool enabled);
  bool is_refreshing() const { return this->refreshing_; }
  int download_progress() const;
  template<typename F> void add_on_progress_callback(F &&callback) {
    this->on_progress_.add(std::forward<F>(callback));
  }
  bool cancel_install();
  uint8_t queue_left() const { return this->queue_count_ - this->queue_pos_; }

  template<typename F> void for_each_repository(F &&f) {
    if (this->repos_ == nullptr)
      return;
    for (size_t i = 0; i < REPO_MAX; i++) {
      if (this->repos_[i].used)
        f(static_cast<const Repository &>(this->repos_[i]));
    }
  }

  template<typename F> void for_each_available(F &&f) {
    if (this->available_ == nullptr)
      return;
    this->keep_available_();
    for (size_t i = 0; i < this->available_count_; i++) {
      const AvailablePackage &p = this->available_[i];
      f(p, static_cast<const Repository &>(this->repos_[p.repo]));
    }
  }
#endif

  template<typename F> void for_each(F &&f) {
    if (!this->open_())
      return;
    for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
      if (this->packages_[i].used)
        f(static_cast<const Package &>(this->packages_[i]));
    }
  }

  int setting_get(const Package &pkg, const char *key, char *out, size_t len);
  int setting_set(const Package &pkg, const char *key, const char *value);
  uintptr_t find_symbol(const char *name);

 protected:
  void changed_() { this->on_change_.call(); }
#ifdef USE_PACKAGE_MANAGER_MQTT
  void setup_mqtt_();
  void mqtt_command_(JsonObject root);
  void mqtt_state_();
#endif
  bool install_(const std::string &source, const std::string &name, const std::string &version, uint32_t abi,
                SettingsTarget settings, const std::string &target, const uint8_t *sha256);
  void install_ended_(bool ok);
  bool refuse_(const char *format, ...) __attribute__((format(printf, 2, 3)));
  Package *installed_or_refuse_(const std::string &name);
  bool open_();
  Package *find_(const char *name);
  void files_save_(const Package &pkg, const char *files);
  void files_erase_(const Package &pkg);
  bool write_entry_(Package &pkg);
  bool start_(Package &pkg);
  bool request_stop_(Package &pkg, AfterStop after);
  void finish_stop_(Package &pkg);
  void after_stop_(Package &pkg);
  void undo_registrations_(Package &pkg);
  storage::PathStorage *storage_at_(const char *dir, char *rel, size_t len, const char *file);
  void installed_(storage::StorageError err, const char *why = nullptr);
  void unpack_start_(storage::PathStorage *ps, const char *rel);
  void unpack_read_();
  void unpack_step_();
  void unpack_write_();
  void unpack_finish_(storage::StorageError err, const char *why);
  void commit_install_();
  bool control_check_();
  void need_lines_(const uint8_t *data, size_t len, bool end);

  PackageStore store_;
  PackageLoader loader_;
  Package *packages_{nullptr};
  PackageUnpack *up_{nullptr};
  bool installing_{false};
#ifdef USE_PACKAGE_MANAGER_HTTP
  storage::StorageError download_(const std::string &url, storage::PathStorage *dst, const char *rel,
                                  const uint8_t *sha256);
  void read_download_();
  void download_done_(storage::StorageError err);
  bool repos_alloc_();
  Repository *find_repo_(const char *name);
  Repository *free_repo_();
  void load_repositories_();
  bool save_repositories_();
  void refresh_next_();
  void open_repo_index_();
  bool open_signature_();
  bool open_small_(const std::string &url, uint8_t what);
  bool signature_read_();
  bool signature_ok_();
  bool open_index_(const std::string &url, bool gz);
  void read_index_();
  bool index_input_(bool final);
  void index_bytes_(const uint8_t *data, size_t len);
  void index_line_();
  void index_read_(bool ok);
  void free_reader_();
  AvailablePackage *index_slot_();
  void end_paragraph_(bool arch_ok, uint8_t repo);
  void refresh_done_();
  void drop_available_(uint8_t repo);
  void keep_available_();
  void release_available_();
  AvailablePackage *find_available_(const char *name);
  bool queued_(const char *name);
  bool queue_add_(const char *name, uint8_t depth);
  bool queue_next_();
  void queue_end_();

  http_request::HttpRequestComponent *http_{nullptr};
  std::shared_ptr<http_request::HttpContainer> container_;
  storage::StreamHandle stream_{};
  PackageDownload *dl_{nullptr};
  uint32_t last_data_time_{0};
  Repository *repos_{nullptr};
  AvailablePackage *available_{nullptr};
  IndexReader *reader_{nullptr};
  ESPPreferenceObject repos_pref_;
  uint16_t available_count_{0};
  uint16_t available_cap_{0};
  uint8_t refresh_repo_{0};
  bool refreshing_{false};
  bool cancel_{false};
  bool download_only_{false};
  int8_t progress_shown_{-1};
  LazyCallbackManager<void(int)> on_progress_;
  uint8_t queue_count_{0};
  uint8_t queue_pos_{0};
  char (*queue_)[sizeof(StoreEntry::name)]{nullptr};
#endif

  const char *directory_{nullptr};
  uint8_t max_packages_{8};
  uint8_t running_{0};
  uint8_t stopping_{0};
  char pending_name_[sizeof(StoreEntry::name)]{};
  char pending_version_[sizeof(StoreEntry::version)]{};
  char pending_file_[storage::STORAGE_VFS_PATH_MAX]{};
  uint32_t pending_abi_{0};
  char last_error_[96]{};
  SettingsTarget pending_settings_{SettingsTarget::PREFERENCES};
  LazyCallbackManager<void()> on_change_;
};

template<typename... Ts> class LoadAction : public Action<Ts...>, public Parented<PackageManager> {
 public:
  void play(const Ts &...x) override { this->parent_->load(); }
};

template<typename... Ts> class InstallAction : public Action<Ts...>, public Parented<PackageManager> {
 public:
  TEMPLATABLE_VALUE(std::string, source)
  TEMPLATABLE_VALUE(std::string, name)
  TEMPLATABLE_VALUE(std::string, version)
  TEMPLATABLE_VALUE(uint32_t, abi)
  TEMPLATABLE_VALUE(std::string, target)
  void set_settings(SettingsTarget settings) { this->settings_ = settings; }
  void play(const Ts &...x) override {
    this->parent_->install(this->source_.value(x...), this->name_.value(x...), this->version_.value(x...),
                           this->abi_.value_or(x..., 0), this->settings_, this->target_.value_or(x..., ""));
  }

 protected:
  SettingsTarget settings_{SettingsTarget::PREFERENCES};
};

template<typename... Ts> class EnableAction : public Action<Ts...>, public Parented<PackageManager> {
 public:
  TEMPLATABLE_VALUE(std::string, name)
  void set_enabled(bool enabled) { this->enabled_ = enabled; }
  void play(const Ts &...x) override { this->parent_->set_enabled(this->name_.value(x...), this->enabled_); }

 protected:
  bool enabled_{true};
};

template<typename... Ts> class RecoveryAction : public Action<Ts...>, public Parented<PackageManager> {
 public:
  TEMPLATABLE_VALUE(std::string, name)
  void set_recovery(bool recovery) { this->recovery_ = recovery; }
  void play(const Ts &...x) override { this->parent_->set_recovery(this->name_.value(x...), this->recovery_); }

 protected:
  bool recovery_{true};
};

template<typename... Ts> class RemoveAction : public Action<Ts...>, public Parented<PackageManager> {
 public:
  TEMPLATABLE_VALUE(std::string, name)
  void play(const Ts &...x) override { this->parent_->remove(this->name_.value(x...)); }
};

}

#endif
