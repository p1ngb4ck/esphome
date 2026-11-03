#include "package_manager.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include "ids.h"
#include "esphome/core/hal.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#ifdef USE_SAFE_MODE_SPLIT
#include "esphome/components/safe_mode/safe_mode.h"
#endif
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#ifdef USE_PACKAGE_MANAGER_HTTP
#include <miniz.h>
#include <sodium.h>
#endif
#include <sdkconfig.h>
#include <strings.h>
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <nvs.h>

extern "C" __attribute__((weak)) uintptr_t component_as_lib_find_symbol(const char *name);
extern "C" __attribute__((weak)) const uint32_t component_as_lib_abi_id;

namespace esphome::package_manager {

using storage::StorageError;

static const char *const TAG = "package_manager";
static const size_t REL_LEN = 160;
static const size_t SETTING_KEY_MAX = 15;
static const size_t SETTING_VALUE_MAX = 255;
static const uint32_t DATA_PAGE = 0x10000;
static const size_t FILES_MAX = 1024;
static const char *const FILES_KEY = ".files";
#ifdef USE_PACKAGE_MANAGER_HTTP
static const uint32_t AVAILABLE_KEEP_MS = 10 * 60 * 1000;
static const uint8_t GZ_NONE = 0;
static const uint8_t GZ_HEADER = 1;
static const uint8_t GZ_INFLATE = 2;
static const uint8_t GZ_DONE = 3;

struct IndexInflate {
  tinfl_decompressor decompressor;
  uint8_t window[TINFL_LZ_DICT_SIZE];
  size_t window_pos;
};

static const uint8_t FETCH_KEY = 1;
static const uint8_t FETCH_SIGNATURE = 2;

struct IndexSignature {
  sha256::SHA256 sha;
  uint8_t key[40]{};
  bool has_key{false};
  uint8_t signature[64]{};
  char text[200]{};
  size_t len{0};
};
static const char *const AVAILABLE_TIMEOUT = "available";

struct SavedRepository {
  char name[sizeof(Repository::name)];
  char url[sizeof(Repository::url)];
  uint8_t key[sizeof(Repository::key)];
  uint8_t has_key;
  uint8_t enabled;
};

struct SavedRepositories {
  SavedRepository items[REPO_MAX];
};
#endif

static void host_log(const char *msg) { ESP_LOGI("package", "%s", msg); }
static uint32_t host_millis() { return millis(); }
static int host_no_setting_get(void *ctx, const char *key, char *out, size_t len) { return -1; }
static int host_no_setting_set(void *ctx, const char *key, const char *value) { return -1; }
static const module_host_api_t HOST_API = {
    ESPHOME_MODULE_ABI_VERSION, host_log, host_millis, nullptr, host_no_setting_get, host_no_setting_set,
};

static uint32_t core_abi_id() {
  return &component_as_lib_abi_id != nullptr ? component_as_lib_abi_id : ESPHOME_MODULE_ABI_VERSION;
}

static bool safe_mode_active() {
#ifdef USE_SAFE_MODE_SPLIT
  return safe_mode::global_safe_mode_active;
#else
  return false;
#endif
}

static int package_setting_get(void *ctx, const char *key, char *out, size_t len) {
  auto *pkg = static_cast<Package *>(ctx);
  return pkg->manager->setting_get(*pkg, key, out, len);
}

static int package_setting_set(void *ctx, const char *key, const char *value) {
  auto *pkg = static_cast<Package *>(ctx);
  return pkg->manager->setting_set(*pkg, key, value);
}

static uintptr_t package_symbol(const char *name, void *ctx) {
  return static_cast<PackageManager *>(ctx)->find_symbol(name);
}

static Package *recording = nullptr;

static void record_registration(RegistrationUndo undo, void *target, void *first, void *second) {
  Package *pkg = recording;
  if (pkg == nullptr)
    return;
  if (pkg->registration_count == pkg->registration_capacity) {
    const uint16_t capacity = pkg->registration_capacity == 0 ? 16 : pkg->registration_capacity * 2;
    RAMAllocator<PackageRegistration> allocator(RAMAllocator<PackageRegistration>::ALLOC_EXTERNAL);
    PackageRegistration *grown = allocator.allocate(capacity);
    if (grown == nullptr) {
      ESP_LOGE(TAG, "%s: no PSRAM to note a registration", pkg->name);
      return;
    }
    if (pkg->registrations != nullptr) {
      std::memcpy(grown, pkg->registrations, pkg->registration_count * sizeof(PackageRegistration));
      allocator.deallocate(pkg->registrations, pkg->registration_capacity);
    }
    pkg->registrations = grown;
    pkg->registration_capacity = capacity;
  }
  pkg->registrations[pkg->registration_count++] = PackageRegistration{undo, target, first, second};
}

static void record_into(Package *pkg) {
  recording = pkg;
  registration_hook = pkg != nullptr ? record_registration : nullptr;
}

static void copy_field(char *dst, size_t len, const char *src) {
  std::strncpy(dst, src, len - 1);
  dst[len - 1] = '\0';
}

void PackageManager::setup() {
#ifdef USE_PACKAGE_MANAGER_HTTP
  this->load_repositories_();
#endif
#ifdef USE_PACKAGE_MANAGER_MQTT
  this->setup_mqtt_();
#endif
  if (this->running_ == 0 && this->stopping_ == 0)
    this->disable_loop();
}

void PackageManager::loop() {
  if (this->packages_ == nullptr) {
    this->disable_loop();
    return;
  }
  for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
    Package &pkg = this->packages_[i];
    if (pkg.stopping) {
      record_into(&pkg);
      const bool done = pkg.stop() != 0;
      record_into(nullptr);
      if (done)
        this->finish_stop_(pkg);
      continue;
    }
    if (pkg.loop == nullptr)
      continue;
    record_into(&pkg);
    pkg.loop();
    record_into(nullptr);
  }
  if (this->running_ == 0 && this->stopping_ == 0)
    this->disable_loop();
}

void PackageManager::undo_registrations_(Package &pkg) {
  for (uint16_t i = pkg.registration_count; i > 0; i--) {
    const PackageRegistration &entry = pkg.registrations[i - 1];
    entry.undo(entry.target, entry.first, entry.second);
  }
  if (pkg.registrations != nullptr) {
    RAMAllocator<PackageRegistration>(RAMAllocator<PackageRegistration>::ALLOC_EXTERNAL)
        .deallocate(pkg.registrations, pkg.registration_capacity);
  }
  pkg.registrations = nullptr;
  pkg.registration_count = 0;
  pkg.registration_capacity = 0;
}

void PackageManager::dump_config() {
  ESP_LOGCONFIG(TAG, "Package manager:\n  Packages partition: %" PRIu32 " bytes", this->store_.size());
  if (this->packages_ == nullptr)
    return;
  for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
    const Package &p = this->packages_[i];
    if (!p.used)
      continue;
    ESP_LOGCONFIG(TAG,
                  "  %s %s: %s%s\n"
                  "    Flash: %" PRIu32 ", PSRAM: %" PRIu32 ", IRAM: %" PRIu32,
                  p.name, p.version, p.module.ok() ? "running" : (p.enabled ? "enabled" : "disabled"),
                  (p.recovery & RECOVERY_BIT) ? ", recovery" : "", p.size, p.psram, p.iram);
  }
}

storage::PathStorage *PackageManager::storage_at_(const char *dir, char *rel, size_t len, const char *file) {
  const char *base = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(dir, &base);
  if (ps == nullptr || base == nullptr || storage::global_storage_worker->has_active_task_io(ps))
    return nullptr;
  int n = snprintf(rel, len, "%s/%s", base, file);
  return n > 0 && static_cast<size_t>(n) < len ? ps : nullptr;
}

bool PackageManager::refuse_(const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(this->last_error_, sizeof(this->last_error_), format, args);
  va_end(args);
  ESP_LOGW(TAG, "%s", this->last_error_);
  return false;
}

Package *PackageManager::installed_or_refuse_(const std::string &name) {
  this->last_error_[0] = '\0';
  if (!this->open_())
    return nullptr;
  Package *pkg = this->find_(name.c_str());
  if (pkg == nullptr)
    this->refuse_("%s is not installed", name.c_str());
  return pkg;
}

bool PackageManager::open_() {
  if (this->packages_ != nullptr)
    return true;
  if (!this->store_.open())
    return this->refuse_("The package store is not available");
  RAMAllocator<Package> allocator(RAMAllocator<Package>::ALLOC_EXTERNAL);
  Package *packages = allocator.allocate(STORE_MAX_ENTRIES);
  if (packages == nullptr)
    return this->refuse_("No PSRAM for the package table");
  for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
    new (&packages[i]) Package{};
    static_cast<StoreEntry &>(packages[i]) = this->store_.index().entries[i];
    packages[i].manager = this;
  }
  this->packages_ = packages;
  return true;
}

template<typename F> static bool each_depend(const char *list, F &&fn) {
  char name[sizeof(StoreEntry::name)];
  const char *p = list;
  while (*p != '\0') {
    size_t n = 0;
    for (; *p != '\0' && *p != ','; p++) {
      if (n + 1 < sizeof(name))
        name[n++] = *p;
    }
    name[n] = '\0';
    if (*p == ',')
      p++;
    if (n > 0 && !fn(name))
      return false;
  }
  return true;
}

Package *PackageManager::find_(const char *name) {
  for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
    if (this->packages_[i].used && std::strncmp(this->packages_[i].name, name, sizeof(StoreEntry::name)) == 0)
      return &this->packages_[i];
  }
  return nullptr;
}

bool PackageManager::write_entry_(Package &pkg) {
  this->store_.index().entries[&pkg - this->packages_] = static_cast<const StoreEntry &>(pkg);
  if (this->store_.commit())
    return true;
  return this->refuse_("%s: package index not written", pkg.name);
}

uintptr_t PackageManager::find_symbol(const char *name) {
  for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
    if (!this->packages_[i].module.ok())
      continue;
    void *found = this->packages_[i].module.symbol(name);
    if (found != nullptr)
      return reinterpret_cast<uintptr_t>(found);
  }
  return 0;
}

bool PackageManager::start_(Package &pkg) {
  if (pkg.module.ok())
    return true;
  if (package_stop_blocked != nullptr && package_stop_blocked())
    return this->refuse_("%s: not started while the display is paused", pkg.name);
  const uint32_t core_abi = core_abi_id();
  if (pkg.abi != core_abi)
    return this->refuse_("%s: built for ABI %" PRIu32 ", core has %" PRIu32, pkg.name, pkg.abi, core_abi);
  char missing[sizeof(StoreEntry::name)] = "";
  bool missing_installed = false;
  if (!each_depend(pkg.depends, [this, &missing, &missing_installed](const char *dep_name) {
        Package *dep = this->find_(dep_name);
        if (dep != nullptr && dep->module.ok())
          return true;
        copy_field(missing, sizeof(missing), dep_name);
        missing_installed = dep != nullptr;
        return false;
      }))
    return this->refuse_("%s: needs %s, which is %s", pkg.name, missing,
                         missing_installed ? "not running" : "not installed");
  if (!this->loader_.load(this->store_.code_base() + pkg.offset, pkg.length, pkg.module, package_symbol, this)) {
    const char *e = this->loader_.last_error();
    return this->refuse_("%s: load failed: %s", pkg.name, e != nullptr ? e : "unknown");
  }
  pkg.api = HOST_API;
  pkg.api.ctx = &pkg;
  pkg.api.setting_get = package_setting_get;
  pkg.api.setting_set = package_setting_set;
  auto init = reinterpret_cast<module_init_fn>(pkg.module.symbol(MODULE_SYM_INIT));
  pkg.stop = reinterpret_cast<module_stop_fn>(pkg.module.symbol(MODULE_SYM_STOP));
  pkg.deinit = reinterpret_cast<module_deinit_fn>(pkg.module.symbol(MODULE_SYM_DEINIT));
  bool ok = init != nullptr && pkg.stop != nullptr && pkg.deinit != nullptr;
  if (ok) {
    record_into(&pkg);
    ok = init(&pkg.api) == ESPHOME_MODULE_ABI_VERSION;
    record_into(nullptr);
  }
  if (!ok) {
    this->undo_registrations_(pkg);
    this->loader_.unload(pkg.module);
    pkg.stop = nullptr;
    pkg.deinit = nullptr;
    return this->refuse_("%s: init failed", pkg.name);
  }
  pkg.loop = reinterpret_cast<module_loop_fn>(pkg.module.symbol(MODULE_SYM_LOOP));
  if (pkg.loop != nullptr && this->running_++ == 0)
    this->enable_loop();
  ESP_LOGI(TAG, "%s %s started", pkg.name, pkg.version);
  return true;
}

bool PackageManager::request_stop_(Package &pkg, AfterStop after) {
  if (!pkg.module.ok()) {
    pkg.after = after;
    this->after_stop_(pkg);
    return true;
  }
  if (pkg.stopping) {
    pkg.after = after;
    return true;
  }
  for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
    const Package &other = this->packages_[i];
    if (&other != &pkg && other.module.ok() && !each_depend(other.depends, [&pkg](const char *dep_name) {
          return std::strncmp(dep_name, pkg.name, sizeof(StoreEntry::name)) != 0;
        }))
      return this->refuse_("%s: not stopped, %s needs it", pkg.name, other.name);
  }
  if (package_stop_blocked != nullptr && package_stop_blocked())
    return this->refuse_("%s: not stopped while the display is paused", pkg.name);
  pkg.after = after;
  pkg.stopping = true;
  this->stopping_++;
  this->enable_loop();
  return true;
}

void PackageManager::finish_stop_(Package &pkg) {
  pkg.stopping = false;
  this->stopping_--;
  this->undo_registrations_(pkg);
  pkg.deinit();
  if (pkg.loop != nullptr)
    this->running_--;
  pkg.loop = nullptr;
  pkg.stop = nullptr;
  pkg.deinit = nullptr;
  this->loader_.unload(pkg.module);
  ESP_LOGI(TAG, "%s stopped", pkg.name);
  this->after_stop_(pkg);
}

void PackageManager::after_stop_(Package &pkg) {
  const AfterStop after = pkg.after;
  pkg.after = AfterStop::NONE;
  switch (after) {
    case AfterStop::DISABLE:
      pkg.enabled = 0;
      this->write_entry_(pkg);
      break;
    case AfterStop::REMOVE:
      ESP_LOGI(TAG, "%s removed", pkg.name);
      this->files_erase_(pkg);
      static_cast<StoreEntry &>(pkg) = StoreEntry{};
      this->write_entry_(pkg);
      break;
    case AfterStop::REPLACE:
      this->commit_install_();
      break;
    case AfterStop::NONE:
      break;
  }
  this->changed_();
}

bool PackageManager::load() {
  if (!this->open_())
    return false;
  bool progress = true;
  while (progress) {
    progress = false;
    for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
      Package &p = this->packages_[i];
      if (p.used && p.enabled && !p.module.ok() && !p.stopping && (!safe_mode_active() || (p.recovery & RECOVERY_BIT)) &&
          this->start_(p))
        progress = true;
    }
  }
  for (size_t i = 0; i < STORE_MAX_ENTRIES; i++) {
    Package &p = this->packages_[i];
    if (p.used && p.enabled && !p.module.ok() && !p.stopping && (!safe_mode_active() || (p.recovery & RECOVERY_BIT))) {
      p.enabled = 0;
      this->write_entry_(p);
    }
  }
  this->changed_();
  return true;
}

bool PackageManager::set_enabled(const std::string &name, bool enabled) {
  Package *pkg = this->installed_or_refuse_(name);
  if (pkg == nullptr)
    return false;
  if (this->installing_ && std::strncmp(this->pending_name_, pkg->name, sizeof(StoreEntry::name)) == 0)
    return this->refuse_("%s is being installed", pkg->name);
  if (!enabled)
    return this->request_stop_(*pkg, AfterStop::DISABLE);
  if (!pkg->enabled) {
    pkg->enabled = 1;
    if (!this->write_entry_(*pkg))
      return false;
  }
  const bool ok = (safe_mode_active() && !(pkg->recovery & RECOVERY_BIT)) || this->start_(*pkg);
  this->changed_();
  return ok;
}

bool PackageManager::set_recovery(const std::string &name, bool recovery) {
  Package *pkg = this->installed_or_refuse_(name);
  if (pkg == nullptr)
    return false;
  pkg->recovery = static_cast<uint8_t>((pkg->recovery & HOLD_BIT) | (recovery ? RECOVERY_BIT : 0));
  if (!this->write_entry_(*pkg))
    return false;
  this->changed_();
  return true;
}

bool PackageManager::set_hold(const std::string &name, bool hold) {
  Package *pkg = this->installed_or_refuse_(name);
  if (pkg == nullptr)
    return false;
  pkg->recovery = static_cast<uint8_t>((pkg->recovery & RECOVERY_BIT) | (hold ? HOLD_BIT : 0));
  if (!this->write_entry_(*pkg))
    return false;
  this->changed_();
  return true;
}

uint32_t PackageManager::firmware_id() const { return core_abi_id(); }

bool PackageManager::remove(const std::string &name) {
  Package *pkg = this->installed_or_refuse_(name);
  if (pkg == nullptr)
    return false;
  if (this->installing_ && std::strncmp(this->pending_name_, pkg->name, sizeof(StoreEntry::name)) == 0)
    return this->refuse_("%s is being installed", pkg->name);
  return this->request_stop_(*pkg, AfterStop::REMOVE);
}

bool PackageManager::install(const std::string &source, const std::string &name, const std::string &version,
                             uint32_t abi, SettingsTarget settings, const std::string &target) {
#ifdef USE_PACKAGE_MANAGER_HTTP
  if (this->queue_count_ > 0) {
    this->last_error_[0] = '\0';
    return this->refuse_("%s: not installed, %s is still being installed", name.c_str(), this->pending_name_);
  }
#endif
  return this->install_(source, name, version, abi, settings, target, nullptr);
}

bool PackageManager::install_(const std::string &source, const std::string &name, const std::string &version,
                              uint32_t abi, SettingsTarget settings, const std::string &target, const uint8_t *sha256) {
  this->last_error_[0] = '\0';
  if (this->installing_)
    return this->refuse_("%s: not installed, %s is still being installed", name.c_str(), this->pending_name_);
#ifdef USE_PACKAGE_MANAGER_HTTP
  if (this->refreshing_)
    return this->refuse_("%s: not installed, the repositories are being refreshed", name.c_str());
#endif
  if (name.empty() || name.size() >= sizeof(StoreEntry::name) || name.find_first_of("/\t\r\n") != std::string::npos)
    return this->refuse_("Install refused: invalid package name '%s'", name.c_str());
  if (version.size() >= sizeof(StoreEntry::version))
    return this->refuse_("%s: install refused, version '%s' too long", name.c_str(), version.c_str());
  if (!target.empty() && target[0] != '/')
    return this->refuse_("%s: install refused, target '%s' is not an absolute path", name.c_str(), target.c_str());
  if (source.empty())
    return this->refuse_("%s: install refused, no source", name.c_str());
  if (!this->open_())
    return false;
  const Package *held = this->find_(name.c_str());
  if (held != nullptr && (held->recovery & HOLD_BIT))
    return this->refuse_("%s is held, not installed again", name.c_str());
  copy_field(this->pending_name_, sizeof(this->pending_name_), name.c_str());
  copy_field(this->pending_version_, sizeof(this->pending_version_), version.c_str());
  this->pending_abi_ = abi != 0 ? abi : core_abi_id();
  this->pending_settings_ = settings;
  if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
#ifdef USE_PACKAGE_MANAGER_HTTP
    const char *dir = target.empty() ? this->directory_ : target.c_str();
    if (dir == nullptr)
      return this->refuse_("%s: no directory for the download", name.c_str());
    char file[sizeof(StoreEntry::name) + 4];
    snprintf(file, sizeof(file), "%s.ipk", name.c_str());
    char rel[REL_LEN];
    storage::PathStorage *dst = this->storage_at_(dir, rel, sizeof(rel), file);
    int n = snprintf(this->pending_file_, sizeof(this->pending_file_), "%s/%s", dir, file);
    if (dst == nullptr || n <= 0 || static_cast<size_t>(n) >= sizeof(this->pending_file_))
      return this->refuse_("%s: install refused, no storage for %s/%s", name.c_str(), dir, file);
    StorageError err = this->download_(source, dst, rel, sha256);
    if (err != StorageError::STORAGE_ERROR_OK)
      return this->refuse_("%s: download not started: %s", name.c_str(), storage::error_to_string(err));
    this->installing_ = true;
    this->changed_();
    return true;
#else
    return this->refuse_("%s: no http_request for a URL source", name.c_str());
#endif
  }
  if (source.size() >= sizeof(this->pending_file_))
    return this->refuse_("%s: install refused, source path too long", name.c_str());
  copy_field(this->pending_file_, sizeof(this->pending_file_), source.c_str());
  this->installing_ = true;
  this->changed_();
  this->defer([this]() { this->installed_(StorageError::STORAGE_ERROR_OK); });
  return true;
}

#ifdef USE_PACKAGE_MANAGER_HTTP
StorageError PackageManager::download_(const std::string &url, storage::PathStorage *dst, const char *rel,
                                       const uint8_t *sha256) {
  this->container_ = this->http_->get(url);
  if (this->container_ == nullptr || this->container_->status_code != 200) {
    if (this->container_ != nullptr) {
      ESP_LOGE(TAG, "Download HTTP %d", this->container_->status_code);
      this->container_->end();
      this->container_.reset();
    }
    return StorageError::STORAGE_ERROR_NOT_FOUND;
  }
  void *mem = RAMAllocator<PackageDownload>(RAMAllocator<PackageDownload>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr) {
    this->dl_ = new (mem) PackageDownload();
    if (sha256 != nullptr) {
      std::memcpy(this->dl_->expected, sha256, sizeof(this->dl_->expected));
      this->dl_->check = true;
      this->dl_->sha.init();
    }
  } else {
    ESP_LOGE(TAG, "No PSRAM for the %u B download chunk", static_cast<unsigned>(sizeof(PackageDownload)));
  }
  StorageError err = this->dl_ == nullptr ? StorageError::STORAGE_ERROR_NO_SPACE
                                             : storage::global_storage_worker->begin_write(
                                                   dst, rel, &this->stream_, [this](StorageError e) {
                                                     if (e != StorageError::STORAGE_ERROR_OK) {
                                                       this->stream_ = storage::StreamHandle{};
                                                       this->download_done_(e);
                                                       return;
                                                     }
                                                     this->last_data_time_ = millis();
                                                     this->defer([this]() { this->read_download_(); });
                                                   });
  if (err != StorageError::STORAGE_ERROR_OK) {
    if (this->dl_ != nullptr) {
      this->dl_->~PackageDownload();
      RAMAllocator<PackageDownload>(RAMAllocator<PackageDownload>::ALLOC_EXTERNAL).deallocate(this->dl_, 1);
    }
    this->dl_ = nullptr;
    this->container_->end();
    this->container_.reset();
  }
  return err;
}

void PackageManager::read_download_() {
  if (this->cancel_) {
    this->download_done_(StorageError::STORAGE_ERROR_NOT_READY);
    return;
  }
  size_t want = sizeof(this->dl_->chunk);
  if (this->container_->content_length > 0) {
    const size_t left = this->container_->content_length - this->container_->get_bytes_read();
    if (left == 0) {
      this->download_done_(StorageError::STORAGE_ERROR_OK);
      return;
    }
    want = std::min<size_t>(want, left);
  }
  const int n = this->container_->read(this->dl_->chunk, want);
  const auto result = http_request::http_read_loop_result(n, this->last_data_time_, this->http_->get_timeout(),
                                                          this->container_->is_read_complete());
  if (result == http_request::HttpReadLoopResult::RETRY) {
    this->defer([this]() { this->read_download_(); });
    return;
  }
  if (result == http_request::HttpReadLoopResult::COMPLETE) {
    this->download_done_(StorageError::STORAGE_ERROR_OK);
    return;
  }
  if (result != http_request::HttpReadLoopResult::DATA) {
    this->download_done_(result == http_request::HttpReadLoopResult::TIMEOUT ? StorageError::STORAGE_ERROR_TIMEOUT
                                                                             : StorageError::STORAGE_ERROR_READ_ERROR);
    return;
  }
  if (this->dl_->check)
    this->dl_->sha.add(this->dl_->chunk, static_cast<size_t>(n));
  const int percent = this->download_progress();
  if (percent >= 0 && percent != this->progress_shown_) {
    this->progress_shown_ = static_cast<int8_t>(percent);
    this->on_progress_.call(percent);
  }
  StorageError err = storage::global_storage_worker->write_chunk(
      this->stream_, this->dl_->chunk, static_cast<size_t>(n), [this](StorageError e) {
        if (e != StorageError::STORAGE_ERROR_OK) {
          this->download_done_(e);
          return;
        }
        this->defer([this]() { this->read_download_(); });
      });
  if (err != StorageError::STORAGE_ERROR_OK)
    this->download_done_(err);
}

void PackageManager::download_done_(StorageError err) {
  if (this->container_ != nullptr) {
    this->container_->end();
    this->container_.reset();
  }
  const char *why = nullptr;
  this->progress_shown_ = -1;
  if (this->cancel_) {
    why = "cancelled";
    err = StorageError::STORAGE_ERROR_NOT_READY;
    this->cancel_ = false;
  }
  if (this->dl_ != nullptr) {
    if (err == StorageError::STORAGE_ERROR_OK && this->dl_->check) {
      this->dl_->sha.calculate();
      if (!this->dl_->sha.equals_bytes(this->dl_->expected)) {
        why = "checksum differs from the repository index";
        err = StorageError::STORAGE_ERROR_READ_ERROR;
      }
    }
    this->dl_->~PackageDownload();
    RAMAllocator<PackageDownload>(RAMAllocator<PackageDownload>::ALLOC_EXTERNAL).deallocate(this->dl_, 1);
    this->dl_ = nullptr;
  }
  if (this->stream_.generation == 0) {
    this->installed_(err, why);
    return;
  }
  StorageError closed = storage::global_storage_worker->end_write(this->stream_, [this, err, why](StorageError e) {
    this->installed_(err != StorageError::STORAGE_ERROR_OK ? err : e, why);
  });
  this->stream_ = storage::StreamHandle{};
  if (closed != StorageError::STORAGE_ERROR_OK)
    this->installed_(err != StorageError::STORAGE_ERROR_OK ? err : closed, why);
}
#endif

void PackageManager::installed_(StorageError err, const char *why) {
#ifdef USE_PACKAGE_MANAGER_HTTP
  if (this->download_only_) {
    this->download_only_ = false;
    this->installing_ = false;
    if (err != StorageError::STORAGE_ERROR_OK) {
      snprintf(this->last_error_, sizeof(this->last_error_), "%s: download failed: %s", this->pending_name_,
               why != nullptr ? why : storage::error_to_string(err));
      ESP_LOGW(TAG, "%s", this->last_error_);
    } else {
      ESP_LOGI(TAG, "%s saved to %s", this->pending_name_, this->pending_file_);
    }
    this->changed_();
    return;
  }
#endif
  const char *rel = nullptr;
  storage::PathStorage *ps = err == StorageError::STORAGE_ERROR_OK
                                 ? storage::global_storage_registry->resolve_path(this->pending_file_, &rel)
                                 : nullptr;
  if (ps == nullptr || rel == nullptr || storage::global_storage_worker->has_active_task_io(ps)) {
    if (why == nullptr && err == StorageError::STORAGE_ERROR_OK)
      why = "package file not readable";
    this->unpack_finish_(err != StorageError::STORAGE_ERROR_OK ? err : StorageError::STORAGE_ERROR_NOT_FOUND, why);
    return;
  }
  this->unpack_start_(ps, rel);
}

void PackageManager::unpack_start_(storage::PathStorage *ps, const char *rel) {
  void *mem = RAMAllocator<PackageUnpack>(RAMAllocator<PackageUnpack>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    this->up_ = new (mem) PackageUnpack();
  if (this->up_ == nullptr || !this->up_->ipk.begin()) {
    this->unpack_finish_(StorageError::STORAGE_ERROR_NO_SPACE, "no PSRAM to unpack");
    return;
  }
  StoreEntry &entry = this->up_->entry;
  copy_field(entry.name, sizeof(entry.name), this->pending_name_);
  copy_field(entry.version, sizeof(entry.version), this->pending_version_);
  entry.abi = this->pending_abi_;
  entry.settings = this->pending_settings_;
  entry.used = 1;
  entry.enabled = 1;
  StorageError err = storage::global_storage_worker->begin_read(ps, rel, &this->up_->in_stream, [this](StorageError e) {
    if (e != StorageError::STORAGE_ERROR_OK) {
      this->up_->in_stream = storage::StreamHandle{};
      this->unpack_finish_(e, "package not readable");
      return;
    }
    this->defer([this]() { this->unpack_read_(); });
  });
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->up_->in_stream = storage::StreamHandle{};
    this->unpack_finish_(err, "package not readable");
  }
}

void PackageManager::unpack_read_() {
  PackageUnpack *up = this->up_;
  StorageError err = storage::global_storage_worker->read_chunk(
      up->in_stream, up->read_buf, sizeof(up->read_buf), &up->read_len, [this](StorageError e) {
        if (e != StorageError::STORAGE_ERROR_OK) {
          this->unpack_finish_(e, "package read failed");
          return;
        }
        this->up_->read_end = this->up_->read_len < sizeof(this->up_->read_buf);
        this->up_->ipk.feed(this->up_->read_buf, this->up_->read_len, this->up_->read_end);
        this->defer([this]() { this->unpack_step_(); });
      });
  if (err != StorageError::STORAGE_ERROR_OK)
    this->unpack_finish_(err, "package read failed");
}

static const char *data_file_name(const char *path) {
  if (std::strncmp(path, "./", 2) == 0)
    path += 2;
  if (path[0] == '\0' || std::strchr(path, '/') != nullptr || std::strstr(path, "..") != nullptr)
    return nullptr;
  return path;
}

static void files_from_sums(const char *sums, char *out, size_t cap) {
  size_t len = 0;
  out[0] = '\0';
  for (const char *line = sums; *line != '\0';) {
    const char *end = std::strchr(line, '\n');
    const size_t line_len = end != nullptr ? static_cast<size_t>(end - line) : std::strlen(line);
    const char *name = static_cast<const char *>(std::memchr(line, ' ', line_len));
    while (name != nullptr && name < line + line_len && *name == ' ')
      name++;
    if (name != nullptr && name < line + line_len) {
      if (std::strncmp(name, "./", 2) == 0)
        name += 2;
      const size_t n = static_cast<size_t>(line + line_len - name);
      if (n > 0 && len + n + 2 <= cap) {
        std::memcpy(out + len, name, n);
        len += n;
        out[len++] = '\n';
        out[len] = '\0';
      }
    }
    if (end == nullptr)
      break;
    line = end + 1;
  }
}

static bool is_shared_object(const char *path) {
  const char *name = data_file_name(path);
  const size_t len = name != nullptr ? std::strlen(name) : 0;
  return len > 3 && std::strcmp(name + len - 3, ".so") == 0;
}

void PackageManager::unpack_step_() {
  while (true) {
    switch (this->up_->ipk.next()) {
      case IpkReader::Event::NEED_INPUT:
        this->unpack_read_();
        return;
      case IpkReader::Event::ERROR:
        this->unpack_finish_(StorageError::STORAGE_ERROR_CORRUPT, this->up_->ipk.error());
        return;
      case IpkReader::Event::DONE: {
        if (!this->up_->data_done) {
          this->unpack_finish_(StorageError::STORAGE_ERROR_INVALID_ARGS, "no package code in the .ipk");
          return;
        }
        StorageError err = storage::global_storage_worker->end_read(this->up_->in_stream, [this](StorageError) {
          this->up_->in_stream = storage::StreamHandle{};
          this->unpack_finish_(StorageError::STORAGE_ERROR_OK, nullptr);
        });
        if (err != StorageError::STORAGE_ERROR_OK) {
          this->up_->in_stream = storage::StreamHandle{};
          this->unpack_finish_(StorageError::STORAGE_ERROR_OK, nullptr);
        }
        return;
      }
      case IpkReader::Event::FILE_DATA:
        if (this->up_->ipk.member() == IpkReader::Member::CONTROL) {
          char *dst = nullptr;
          size_t *len = nullptr;
          size_t cap = 0;
          if (std::strcmp(this->up_->ipk.path(), "./control") == 0) {
            dst = this->up_->control;
            len = &this->up_->control_len;
            cap = sizeof(this->up_->control);
          } else if (std::strcmp(this->up_->ipk.path(), "./sha256sums") == 0) {
            dst = this->up_->sums;
            len = &this->up_->sums_len;
            cap = sizeof(this->up_->sums);
          }
          if (dst == nullptr) {
            const bool needs = std::strcmp(this->up_->ipk.path(), "./needs") == 0;
            if (needs || std::strcmp(this->up_->ipk.path(), "./ids") == 0) {
              this->up_->line_kind = needs ? 1 : 2;
              this->need_lines_(this->up_->ipk.chunk(), this->up_->ipk.chunk_len(), false);
            }
            break;
          }
          if (*len + this->up_->ipk.chunk_len() >= cap) {
            this->unpack_finish_(StorageError::STORAGE_ERROR_TRANSFER_TOO_LARGE, "control file too large");
            return;
          }
          std::memcpy(dst + *len, this->up_->ipk.chunk(), this->up_->ipk.chunk_len());
          *len += this->up_->ipk.chunk_len();
          dst[*len] = '\0';
          break;
        }
        if (this->up_->ipk.member() != IpkReader::Member::DATA || !is_shared_object(this->up_->ipk.path()))
          break;
        if (this->up_->entry.size == 0) {
          this->unpack_finish_(StorageError::STORAGE_ERROR_INVALID_ARGS, "data before control");
          return;
        }
        if (this->up_->missing != 0) {
          this->unpack_finish_(StorageError::STORAGE_ERROR_INVALID_ARGS, "the core lacks what the package needs");
          return;
        }
        if (this->up_->data_open && this->up_->data_done) {
          this->unpack_finish_(StorageError::STORAGE_ERROR_INVALID_ARGS, "more than one package code file");
          return;
        }
        if (!this->up_->data_open) {
          this->up_->sha.init();
          this->up_->data_open = true;
        }
        this->up_->chunk = this->up_->ipk.chunk();
        this->up_->chunk_left = this->up_->ipk.chunk_len();
        this->unpack_write_();
        return;
      case IpkReader::Event::FILE_END:
        if (this->up_->ipk.member() == IpkReader::Member::CONTROL) {
          if (std::strcmp(this->up_->ipk.path(), "./control") == 0 && !this->control_check_()) {
            this->unpack_finish_(StorageError::STORAGE_ERROR_INVALID_ARGS, "package does not fit this device");
            return;
          }
          if (this->up_->line_kind != 0) {
            this->need_lines_(nullptr, 0, true);
            this->up_->line_kind = 0;
          }
          break;
        }
        if (this->up_->ipk.member() != IpkReader::Member::DATA || !this->up_->data_open || this->up_->data_done)
          break;
        {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
          this->up_->sha.calculate();
          char want[80];
          int n = snprintf(want, sizeof(want), "  ./%s\n", data_file_name(this->up_->ipk.path()));
          const char *line = n > 0 ? std::strstr(this->up_->sums, want) : nullptr;
          if (line == nullptr || line - this->up_->sums < 64 || !this->up_->sha.equals_hex(line - 64)) {
            this->unpack_finish_(StorageError::STORAGE_ERROR_VERIFY_MISMATCH, "checksum mismatch");
            return;
          }
#endif
          this->up_->entry.length = this->up_->written;
          this->up_->data_done = true;
        }
        break;
    }
  }
}

void PackageManager::unpack_write_() {
  PackageUnpack *up = this->up_;
  const size_t n = std::min<size_t>(up->chunk_left, STORE_SECTOR);
  if (up->written + n > up->entry.size) {
    this->unpack_finish_(StorageError::STORAGE_ERROR_TRANSFER_TOO_LARGE, "package code larger than it reports");
    return;
  }
  if (!this->store_.write(up->entry.offset, up->written, up->chunk, n)) {
    this->unpack_finish_(StorageError::STORAGE_ERROR_WRITE_ERROR, "packages partition write failed");
    return;
  }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  up->sha.add(up->chunk, n);
#endif
  up->chunk += n;
  up->chunk_left -= n;
  up->written += n;
  if (up->chunk_left > 0) {
    this->defer([this]() { this->unpack_write_(); });
    return;
  }
  this->defer([this]() { this->unpack_step_(); });
}

void PackageManager::need_lines_(const uint8_t *data, size_t len, bool end) {
  PackageUnpack *up = this->up_;
  auto check = [this, up]() {
    up->line[up->line_len] = '\0';
    up->line_len = 0;
    if (up->line[0] == '\0')
      return;
    bool found;
    if (up->line_kind == 1) {
      found = (component_as_lib_find_symbol != nullptr && component_as_lib_find_symbol(up->line) != 0) ||
              this->find_symbol(up->line) != 0;
    } else {
      char *type = std::strchr(up->line, ' ');
      if (type != nullptr)
        *type++ = '\0';
      found = type != nullptr && query(up->line, strtoul(type, nullptr, 10)) != nullptr;
    }
    if (!found && up->missing++ < 8)
      ESP_LOGE(TAG, "%s: the core has no %s %s", this->pending_name_, up->line_kind == 1 ? "symbol" : "id", up->line);
  };
  for (size_t i = 0; i < len; i++) {
    const char ch = static_cast<char>(data[i]);
    if (ch == '\n') {
      check();
    } else if (up->line_len + 1 < sizeof(up->line)) {
      up->line[up->line_len++] = ch;
    }
  }
  if (end && up->line_len > 0)
    check();
}

bool PackageManager::control_check_() {
  StoreEntry &entry = this->up_->entry;
  char package[sizeof(StoreEntry::name)];
  size_t i = 0;
  for (; this->pending_name_[i] != '\0' && i + 1 < sizeof(package); i++) {
    const char c = this->pending_name_[i];
    package[i] = c == '_' ? '-' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  package[i] = '\0';
  bool package_ok = false;
  bool arch_ok = false;
  uint32_t flash = 0;
  char *save = nullptr;
  for (char *line = strtok_r(this->up_->control, "\n", &save); line != nullptr; line = strtok_r(nullptr, "\n", &save)) {
    char *value = std::strchr(line, ':');
    if (value == nullptr)
      continue;
    *value++ = '\0';
    while (*value == ' ')
      value++;
    if (std::strcmp(line, "Package") == 0) {
      package_ok = std::strcmp(value, package) == 0;
    } else if (std::strcmp(line, "Architecture") == 0) {
      arch_ok = std::strcmp(value, CONFIG_IDF_TARGET) == 0;
    } else if (std::strcmp(line, "Version") == 0) {
      copy_field(entry.version, sizeof(entry.version), value);
    } else if (std::strcmp(line, "Depends") == 0) {
      size_t j = 0;
      for (; *value != '\0' && j + 1 < sizeof(entry.depends); value++) {
        if (*value != ' ')
          entry.depends[j++] = *value == '-' ? '_' : *value;
      }
      entry.depends[j] = '\0';
    } else if (std::strcmp(line, "ESPHome-ABI") == 0) {
      entry.abi = strtoul(value, nullptr, 10);
    } else if (std::strcmp(line, "ESPHome-Flash") == 0) {
      flash = strtoul(value, nullptr, 10);
    } else if (std::strcmp(line, "ESPHome-PSRAM") == 0) {
      entry.psram = strtoul(value, nullptr, 10);
    } else if (std::strcmp(line, "ESPHome-IRAM") == 0) {
      entry.iram = strtoul(value, nullptr, 10);
    }
  }
  const char *name = this->pending_name_;
  if (!package_ok || !arch_ok) {
    ESP_LOGE(TAG, "%s: package %s, architecture %s", name, package_ok ? "ok" : "differs", arch_ok ? "ok" : "differs");
    return false;
  }
  if (entry.abi != core_abi_id()) {
    ESP_LOGE(TAG, "%s: built for ABI %" PRIu32 ", core has %" PRIu32, name, entry.abi, core_abi_id());
    return false;
  }
  if (flash == 0) {
    ESP_LOGE(TAG, "%s: the package reports no flash size", name);
    return false;
  }
  if (entry.iram != 0) {
    ESP_LOGE(TAG, "%s: needs %" PRIu32 " bytes of internal RAM sections, the loader places none", name, entry.iram);
    return false;
  }
  const size_t psram = ((entry.psram + DATA_PAGE - 1) / DATA_PAGE + 1) * DATA_PAGE;
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < psram) {
    ESP_LOGE(TAG, "%s: needs %u bytes of PSRAM, %u free", name, static_cast<unsigned>(psram),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return false;
  }
  if (!each_depend(entry.depends, [this, name](const char *dep_name) {
        if (this->find_(dep_name) != nullptr)
          return true;
        ESP_LOGE(TAG, "%s: needs %s installed first", name, dep_name);
        return false;
      }))
    return false;
  if (this->find_(name) == nullptr) {
    size_t installed = 0;
    for (size_t j = 0; j < STORE_MAX_ENTRIES; j++)
      installed += this->packages_[j].used ? 1 : 0;
    if (installed >= this->max_packages_) {
      ESP_LOGE(TAG, "%s: no free package slot", name);
      return false;
    }
  }
  uint32_t offset = 0;
  if (!this->store_.allocate(flash, offset)) {
    ESP_LOGE(TAG, "%s: needs %" PRIu32 " bytes, no room in the packages partition", name, flash);
    return false;
  }
  entry.offset = offset;
  entry.size = (flash + STORE_SECTOR - 1) / STORE_SECTOR * STORE_SECTOR;
  return true;
}

void PackageManager::unpack_finish_(StorageError err, const char *why) {
  if (this->up_ != nullptr && this->up_->in_stream.generation != 0) {
    storage::global_storage_worker->end_read(this->up_->in_stream, [](StorageError) {});
    this->up_->in_stream = storage::StreamHandle{};
  }
  if (this->up_ != nullptr && err == StorageError::STORAGE_ERROR_OK && this->up_->data_done) {
    Package *old = this->find_(this->up_->entry.name);
    if (old != nullptr && (old->module.ok() || old->stopping)) {
      if (this->request_stop_(*old, AfterStop::REPLACE))
        return;
      why = "the installed version keeps running";
      err = StorageError::STORAGE_ERROR_NOT_READY;
    } else {
      this->commit_install_();
      return;
    }
  }
  snprintf(this->last_error_, sizeof(this->last_error_), "%s: install failed: %s", this->pending_name_,
           why != nullptr ? why : storage::error_to_string(err));
  ESP_LOGE(TAG, "%s", this->last_error_);
  if (this->up_ != nullptr) {
    this->up_->ipk.end();
    this->up_->~PackageUnpack();
    RAMAllocator<PackageUnpack>(RAMAllocator<PackageUnpack>::ALLOC_EXTERNAL).deallocate(this->up_, 1);
    this->up_ = nullptr;
  }
  this->installing_ = false;
  this->install_ended_(false);
}

void PackageManager::commit_install_() {
  if (this->up_ == nullptr)
    return;
  StoreEntry entry = this->up_->entry;
  RAMAllocator<char> chars(RAMAllocator<char>::ALLOC_EXTERNAL);
  char *files = chars.allocate(FILES_MAX);
  if (files != nullptr)
    files_from_sums(this->up_->sums, files, FILES_MAX);
  this->up_->ipk.end();
  this->up_->~PackageUnpack();
  RAMAllocator<PackageUnpack>(RAMAllocator<PackageUnpack>::ALLOC_EXTERNAL).deallocate(this->up_, 1);
  this->up_ = nullptr;
  this->installing_ = false;
  Package *pkg = this->find_(entry.name);
  if (pkg != nullptr) {
    entry.recovery = pkg->recovery;
  } else {
    for (size_t i = 0; i < STORE_MAX_ENTRIES && pkg == nullptr; i++) {
      if (!this->packages_[i].used)
        pkg = &this->packages_[i];
    }
  }
  if (pkg == nullptr) {
    ESP_LOGE(TAG, "%s: no free package slot", entry.name);
    if (files != nullptr)
      chars.deallocate(files, FILES_MAX);
    this->install_ended_(false);
    return;
  }
  static_cast<StoreEntry &>(*pkg) = entry;
  if (!this->write_entry_(*pkg)) {
    if (files != nullptr)
      chars.deallocate(files, FILES_MAX);
    this->install_ended_(false);
    return;
  }
  if (files != nullptr) {
    this->files_save_(*pkg, files);
    chars.deallocate(files, FILES_MAX);
  }
  this->store_.sync_code(entry.offset, entry.size);
  ESP_LOGI(TAG, "%s %s installed", pkg->name, pkg->version);
  if (!safe_mode_active() || (pkg->recovery & RECOVERY_BIT))
    this->start_(*pkg);
  this->install_ended_(true);
}

void PackageManager::install_ended_(bool ok) {
  this->changed_();
#ifdef USE_PACKAGE_MANAGER_HTTP
  if (this->queue_count_ == 0)
    return;
  if (!ok) {
    this->queue_end_();
    return;
  }
  this->defer([this]() { this->queue_next_(); });
#endif
}

#ifdef USE_PACKAGE_MANAGER_HTTP
bool PackageManager::repos_alloc_() {
  if (this->repos_ != nullptr)
    return true;
  Repository *repos = RAMAllocator<Repository>(RAMAllocator<Repository>::ALLOC_EXTERNAL).allocate(REPO_MAX);
  if (repos == nullptr)
    return this->refuse_("No PSRAM for the repository list");
  for (size_t i = 0; i < REPO_MAX; i++)
    repos[i] = Repository{};
  this->repos_ = repos;
  return true;
}

Repository *PackageManager::find_repo_(const char *name) {
  for (size_t i = 0; this->repos_ != nullptr && i < REPO_MAX; i++) {
    if (this->repos_[i].used && std::strncmp(this->repos_[i].name, name, sizeof(Repository::name)) == 0)
      return &this->repos_[i];
  }
  return nullptr;
}

Repository *PackageManager::free_repo_() {
  for (size_t i = 0; this->repos_ != nullptr && i < REPO_MAX; i++) {
    if (!this->repos_[i].used)
      return &this->repos_[i];
  }
  return nullptr;
}

static bool parse_public_key(const std::string &text, uint8_t *key) {
  size_t end = text.find_last_not_of(" \t\r\n");
  if (end == std::string::npos)
    return false;
  size_t start = text.find_last_of('\n', end);
  start = start == std::string::npos ? 0 : start + 1;
  uint8_t raw[2 + 40];
  if (base64_decode(reinterpret_cast<const uint8_t *>(text.data() + start), end + 1 - start, raw, sizeof(raw)) !=
          sizeof(raw) ||
      raw[0] != 'E' || raw[1] != 'd')
    return false;
  std::memcpy(key, raw + 2, 40);
  return true;
}

void PackageManager::add_repository(const char *name, const char *url, const char *public_key_hex) {
  Repository *repo = this->repos_alloc_() ? this->free_repo_() : nullptr;
  if (repo == nullptr)
    return;
  copy_field(repo->name, sizeof(repo->name), name);
  copy_field(repo->url, sizeof(repo->url), url);
  repo->has_key = public_key_hex[0] != '\0' && parse_hex(public_key_hex, repo->key, sizeof(repo->key)) ? 1 : 0;
  repo->used = 1;
  repo->enabled = 1;
  repo->builtin = 1;
}

void PackageManager::load_repositories_() {
  this->repos_pref_ =
      global_preferences->make_preference<SavedRepositories>(fnv1_hash("package_manager.repositories"), true);
  if (!this->repos_alloc_())
    return;
  void *mem = RAMAllocator<SavedRepositories>(RAMAllocator<SavedRepositories>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr)
    return;
  SavedRepositories *saved = static_cast<SavedRepositories *>(mem);
  std::memset(saved, 0, sizeof(SavedRepositories));
  if (this->repos_pref_.load(saved)) {
    for (const SavedRepository &item : saved->items) {
      if (item.name[0] == '\0' || this->find_repo_(item.name) != nullptr)
        continue;
      Repository *repo = this->free_repo_();
      if (repo == nullptr)
        break;
      copy_field(repo->name, sizeof(repo->name), item.name);
      copy_field(repo->url, sizeof(repo->url), item.url);
      std::memcpy(repo->key, item.key, sizeof(repo->key));
      repo->has_key = item.has_key;
      repo->used = 1;
      repo->enabled = item.enabled;
    }
  }
  RAMAllocator<SavedRepositories>(RAMAllocator<SavedRepositories>::ALLOC_EXTERNAL).deallocate(saved, 1);
}

bool PackageManager::save_repositories_() {
  void *mem = RAMAllocator<SavedRepositories>(RAMAllocator<SavedRepositories>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr)
    return this->refuse_("No PSRAM to save the repositories");
  SavedRepositories *saved = static_cast<SavedRepositories *>(mem);
  std::memset(saved, 0, sizeof(SavedRepositories));
  size_t n = 0;
  for (size_t i = 0; i < REPO_MAX; i++) {
    const Repository &repo = this->repos_[i];
    if (!repo.used || repo.builtin)
      continue;
    SavedRepository &item = saved->items[n++];
    copy_field(item.name, sizeof(item.name), repo.name);
    copy_field(item.url, sizeof(item.url), repo.url);
    std::memcpy(item.key, repo.key, sizeof(item.key));
    item.has_key = repo.has_key;
    item.enabled = repo.enabled;
  }
  const bool ok = this->repos_pref_.save(saved);
  RAMAllocator<SavedRepositories>(RAMAllocator<SavedRepositories>::ALLOC_EXTERNAL).deallocate(saved, 1);
  return ok || this->refuse_("The repositories were not saved");
}

bool PackageManager::repo_add(const std::string &name, const std::string &url, const std::string &public_key) {
  this->last_error_[0] = '\0';
  if (name.empty() || name.size() >= sizeof(Repository::name) || name.find_first_of("\t\r\n") != std::string::npos)
    return this->refuse_("Repository refused: invalid name '%s'", name.c_str());
  size_t len = url.size();
  while (len > 0 && url[len - 1] == '/')
    len--;
  if ((url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) || len >= sizeof(Repository::url))
    return this->refuse_("%s: repository refused, the URL must start with http:// or https:// (at most %u characters)",
                         name.c_str(), static_cast<unsigned>(sizeof(Repository::url) - 1));
  uint8_t key[sizeof(Repository::key)];
  const bool has_key = public_key.find_first_not_of(" \t\r\n") != std::string::npos;
  if (has_key && !parse_public_key(public_key, key))
    return this->refuse_("%s: repository refused, the public key is not a usign / signify Ed25519 key", name.c_str());
  if (this->refreshing_)
    return this->refuse_("%s: not added, the repositories are being refreshed", name.c_str());
  if (!this->repos_alloc_())
    return false;
  if (this->find_repo_(name.c_str()) != nullptr)
    return this->refuse_("Repository %s exists already", name.c_str());
  Repository *repo = this->free_repo_();
  if (repo == nullptr)
    return this->refuse_("%s: not added, %u repositories at most", name.c_str(), static_cast<unsigned>(REPO_MAX));
  *repo = Repository{};
  copy_field(repo->name, sizeof(repo->name), name.c_str());
  std::memcpy(repo->url, url.data(), len);
  repo->url[len] = '\0';
  if (has_key)
    std::memcpy(repo->key, key, sizeof(repo->key));
  repo->has_key = has_key ? 1 : 0;
  repo->used = 1;
  repo->enabled = 1;
  if (!this->save_repositories_()) {
    *repo = Repository{};
    return false;
  }
  ESP_LOGI(TAG, "Repository %s added", repo->name);
  this->changed_();
  return true;
}

bool PackageManager::repo_delete(const std::string &name) {
  this->last_error_[0] = '\0';
  Repository *repo = this->find_repo_(name.c_str());
  if (repo == nullptr)
    return this->refuse_("Repository %s does not exist", name.c_str());
  if (repo->builtin)
    return this->refuse_("Repository %s is built in, it can only be disabled", name.c_str());
  if (this->refreshing_)
    return this->refuse_("%s: not deleted, the repositories are being refreshed", name.c_str());
  *repo = Repository{};
  this->drop_available_(static_cast<uint8_t>(repo - this->repos_));
  if (!this->save_repositories_())
    return false;
  ESP_LOGI(TAG, "Repository %s deleted", name.c_str());
  this->changed_();
  return true;
}

bool PackageManager::repo_enable(const std::string &name, bool enabled) {
  this->last_error_[0] = '\0';
  Repository *repo = this->find_repo_(name.c_str());
  if (repo == nullptr)
    return this->refuse_("Repository %s does not exist", name.c_str());
  if (this->refreshing_)
    return this->refuse_("%s: not changed, the repositories are being refreshed", name.c_str());
  repo->enabled = enabled ? 1 : 0;
  if (!enabled) {
    repo->state = RepoState::NONE;
    repo->count = 0;
    this->drop_available_(static_cast<uint8_t>(repo - this->repos_));
  }
  if (!repo->builtin && !this->save_repositories_())
    return false;
  this->changed_();
  return true;
}

bool PackageManager::refresh() {
  this->last_error_[0] = '\0';
  if (this->http_ == nullptr)
    return this->refuse_("No http_request for the repositories");
  if (this->refreshing_)
    return this->refuse_("The repositories are being refreshed");
  if (this->installing_ || this->queue_count_ > 0)
    return this->refuse_("Not refreshed, %s is being installed", this->pending_name_);
  bool any = false;
  for (size_t i = 0; this->repos_ != nullptr && i < REPO_MAX; i++)
    any = any || (this->repos_[i].used && this->repos_[i].enabled);
  if (!any)
    return this->refuse_("No repository enabled");
  if (this->available_ == nullptr) {
    AvailablePackage *list =
        RAMAllocator<AvailablePackage>(RAMAllocator<AvailablePackage>::ALLOC_EXTERNAL).allocate(AVAILABLE_STEP);
    if (list == nullptr)
      return this->refuse_("No PSRAM for the package list");
    this->available_ = list;
    this->available_cap_ = AVAILABLE_STEP;
  }
  void *mem = RAMAllocator<IndexReader>(RAMAllocator<IndexReader>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr) {
    this->keep_available_();
    return this->refuse_("No PSRAM for the repository index");
  }
  this->reader_ = static_cast<IndexReader *>(mem);
  std::memset(this->reader_, 0, sizeof(IndexReader));
  this->available_count_ = 0;
  this->available_[0] = AvailablePackage{};
  for (size_t i = 0; i < REPO_MAX; i++) {
    this->repos_[i].state = RepoState::NONE;
    this->repos_[i].count = 0;
  }
  this->cancel_timeout(AVAILABLE_TIMEOUT);
  this->refreshing_ = true;
  this->refresh_repo_ = 0;
  this->changed_();
  this->defer([this]() { this->refresh_next_(); });
  return true;
}

void PackageManager::refresh_next_() {
  while (this->refresh_repo_ < REPO_MAX &&
         !(this->repos_[this->refresh_repo_].used && this->repos_[this->refresh_repo_].enabled))
    this->refresh_repo_++;
  if (this->refresh_repo_ >= REPO_MAX) {
    this->refresh_done_();
    return;
  }
  this->reader_->reading_signature = false;
  this->reader_->checking = false;
  const Repository &repo = this->repos_[this->refresh_repo_];
  if (this->reader_->signature == nullptr) {
    void *mem = RAMAllocator<IndexSignature>(RAMAllocator<IndexSignature>::ALLOC_EXTERNAL).allocate(1);
    if (mem != nullptr)
      this->reader_->signature = new (mem) IndexSignature();
  }
  IndexSignature *s = this->reader_->signature;
  if (s == nullptr || sodium_init() < 0) {
    this->open_repo_index_();
    return;
  }
  s->has_key = repo.has_key != 0;
  if (s->has_key)
    std::memcpy(s->key, repo.key, sizeof(s->key));
  if (s->has_key ? this->open_signature_() : this->open_small_(std::string(repo.url) + "/signing.pub", FETCH_KEY))
    return;
  this->open_repo_index_();
}

bool PackageManager::open_signature_() {
  char build[12];
  snprintf(build, sizeof(build), "/%08" PRIx32, core_abi_id());
  std::string url = this->repos_[this->refresh_repo_].url;
  url += build;
  url += "/Packages.sig";
  return this->open_small_(url, FETCH_SIGNATURE);
}

void PackageManager::open_repo_index_() {
  char build[12];
  snprintf(build, sizeof(build), "/%08" PRIx32, core_abi_id());
  std::string url = this->repos_[this->refresh_repo_].url;
  url += build;
  url += "/Packages";
  if (!this->open_index_(url + ".gz", true) && !this->open_index_(url, false)) {
    this->index_read_(false);
    return;
  }
  this->last_data_time_ = millis();
  this->defer([this]() { this->read_index_(); });
}

bool PackageManager::open_small_(const std::string &url, uint8_t what) {
  IndexReader *r = this->reader_;
  r->signature->len = 0;
  this->container_ = this->http_->get(url);
  if (this->container_ == nullptr || this->container_->status_code != 200) {
    if (this->container_ != nullptr) {
      this->container_->end();
      this->container_.reset();
    }
    return false;
  }
  r->reading_signature = true;
  r->fetching = what;
  this->last_data_time_ = millis();
  this->defer([this]() { this->read_index_(); });
  return true;
}

bool PackageManager::signature_read_() {
  IndexSignature *s = this->reader_->signature;
  const Repository &repo = this->repos_[this->refresh_repo_];
  s->text[s->len] = '\0';
  const char *line = std::strchr(s->text, '\n');
  if (line == nullptr)
    return false;
  line++;
  size_t len = 0;
  while (line[len] != '\0' && line[len] != '\n' && line[len] != '\r')
    len++;
  if (!s->has_key)
    return false;
  uint8_t raw[2 + 8 + 64];
  if (base64_decode(reinterpret_cast<const uint8_t *>(line), len, raw, sizeof(raw)) != sizeof(raw) || raw[0] != 'E' ||
      raw[1] != 'd' || std::memcmp(raw + 2, s->key, 8) != 0)
    return false;
  std::memcpy(s->signature, raw + 10, sizeof(s->signature));
  s->sha.init();
  return true;
}

bool PackageManager::signature_ok_() {
  IndexSignature *s = this->reader_->signature;
  uint8_t digest[32];
  s->sha.calculate();
  s->sha.get_bytes(digest);
  return crypto_sign_ed25519_verify_detached(s->signature, digest, sizeof(digest), s->key + 8) == 0;
}

bool PackageManager::open_index_(const std::string &url, bool gz) {
  IndexReader *r = this->reader_;
  r->in_len = 0;
  r->line_len = 0;
  r->arch_ok = false;
  r->gz_state = gz ? GZ_HEADER : GZ_NONE;
  this->container_ = this->http_->get(url);
  if (this->container_ == nullptr || this->container_->status_code != 200) {
    if (this->container_ != nullptr) {
      this->container_->end();
      this->container_.reset();
    }
    return false;
  }
  if (gz && r->inflate == nullptr) {
    void *mem = RAMAllocator<IndexInflate>(RAMAllocator<IndexInflate>::ALLOC_EXTERNAL).allocate(1);
    if (mem == nullptr) {
      this->refuse_("No PSRAM to inflate the repository index");
      this->container_->end();
      this->container_.reset();
      return false;
    }
    r->inflate = static_cast<IndexInflate *>(mem);
  }
  if (gz) {
    tinfl_init(&r->inflate->decompressor);
    r->inflate->window_pos = 0;
  }
  return true;
}

void PackageManager::read_index_() {
  IndexReader *r = this->reader_;
  if (r->reading_signature) {
    IndexSignature *s = r->signature;
    const bool all = this->container_->content_length > 0 &&
                     this->container_->get_bytes_read() >= this->container_->content_length;
    bool readable = true;
    if (!all) {
      const size_t room = sizeof(s->text) - 1 - s->len;
      const int n = room > 0 ? this->container_->read(reinterpret_cast<uint8_t *>(s->text + s->len), room) : -1;
      const auto result = room > 0 ? http_request::http_read_loop_result(n, this->last_data_time_,
                                                                         this->http_->get_timeout(),
                                                                         this->container_->is_read_complete())
                                   : http_request::HttpReadLoopResult::ERROR;
      if (result == http_request::HttpReadLoopResult::RETRY || result == http_request::HttpReadLoopResult::DATA) {
        if (result == http_request::HttpReadLoopResult::DATA)
          s->len += static_cast<size_t>(n);
        this->defer([this]() { this->read_index_(); });
        return;
      }
      readable = result == http_request::HttpReadLoopResult::COMPLETE;
    }
    this->container_->end();
    this->container_.reset();
    r->reading_signature = false;
    if (r->fetching == FETCH_KEY) {
      s->text[s->len] = '\0';
      s->has_key = readable && parse_public_key(s->text, s->key);
      if (s->has_key && this->open_signature_())
        return;
      this->open_repo_index_();
      return;
    }
    r->checking = readable && this->signature_read_();
    this->open_repo_index_();
    return;
  }
  if (this->container_->content_length > 0 &&
      this->container_->get_bytes_read() >= this->container_->content_length) {
    this->index_read_(this->index_input_(true));
    return;
  }
  const int n = this->container_->read(r->in + r->in_len, INDEX_IN_SIZE - r->in_len);
  const auto result = http_request::http_read_loop_result(n, this->last_data_time_, this->http_->get_timeout(),
                                                          this->container_->is_read_complete());
  if (result == http_request::HttpReadLoopResult::RETRY) {
    this->defer([this]() { this->read_index_(); });
    return;
  }
  if (result == http_request::HttpReadLoopResult::COMPLETE) {
    this->index_read_(this->index_input_(true));
    return;
  }
  if (result != http_request::HttpReadLoopResult::DATA) {
    this->index_read_(false);
    return;
  }
  r->in_len += static_cast<size_t>(n);
  if (!this->index_input_(false)) {
    this->index_read_(false);
    return;
  }
  this->defer([this]() { this->read_index_(); });
}

static size_t gzip_header_length(const uint8_t *data, size_t len) {
  if (len < 10)
    return 0;
  if (data[0] != 0x1F || data[1] != 0x8B || data[2] != 8)
    return SIZE_MAX;
  const uint8_t flags = data[3];
  size_t pos = 10;
  if (flags & 0x04) {
    if (pos + 2 > len)
      return 0;
    pos += 2 + (data[pos] | (data[pos + 1] << 8));
  }
  for (const uint8_t text : {static_cast<uint8_t>(0x08), static_cast<uint8_t>(0x10)}) {
    if (!(flags & text))
      continue;
    while (pos < len && data[pos] != 0)
      pos++;
    if (pos >= len)
      return 0;
    pos++;
  }
  if (flags & 0x02)
    pos += 2;
  return pos <= len ? pos : 0;
}

bool PackageManager::index_input_(bool final) {
  IndexReader *r = this->reader_;
  if (r->gz_state == GZ_NONE) {
    this->index_bytes_(r->in, r->in_len);
    r->in_len = 0;
  } else {
    size_t pos = 0;
    if (r->gz_state == GZ_HEADER) {
      const size_t header = gzip_header_length(r->in, r->in_len);
      if (header == SIZE_MAX)
        return false;
      if (header == 0)
        return !final && r->in_len < INDEX_IN_SIZE;
      pos = header;
      r->gz_state = GZ_INFLATE;
    }
    IndexInflate *z = r->inflate;
    while (r->gz_state == GZ_INFLATE) {
      size_t in_size = r->in_len - pos;
      size_t out_size = TINFL_LZ_DICT_SIZE - z->window_pos;
      const tinfl_status status =
          tinfl_decompress(&z->decompressor, r->in + pos, &in_size, z->window, z->window + z->window_pos, &out_size,
                           final ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
      pos += in_size;
      this->index_bytes_(z->window + z->window_pos, out_size);
      z->window_pos = (z->window_pos + out_size) & (TINFL_LZ_DICT_SIZE - 1);
      if (status == TINFL_STATUS_DONE) {
        r->gz_state = GZ_DONE;
        pos = r->in_len;
      } else if (status < 0) {
        return false;
      } else if (status == TINFL_STATUS_NEEDS_MORE_INPUT || (in_size == 0 && out_size == 0)) {
        break;
      }
    }
    std::memmove(r->in, r->in + pos, r->in_len - pos);
    r->in_len -= pos;
  }
  if (final) {
    if (r->line_len > 0)
      this->index_line_();
    this->end_paragraph_(r->arch_ok, this->refresh_repo_);
    r->arch_ok = false;
  }
  return true;
}

void PackageManager::index_bytes_(const uint8_t *data, size_t len) {
  IndexReader *r = this->reader_;
  if (len > 0 && r->checking)
    r->signature->sha.add(data, len);
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '\n') {
      this->index_line_();
    } else if (r->line_len + 1 < INDEX_LINE_SIZE) {
      r->line[r->line_len++] = static_cast<char>(data[i]);
    }
  }
}

void PackageManager::index_read_(bool ok) {
  if (this->container_ != nullptr) {
    this->container_->end();
    this->container_.reset();
  }
  Repository &repo = this->repos_[this->refresh_repo_];
  repo.signed_ok = ok && this->reader_->checking && this->signature_ok_() ? 1 : 0;
  if (ok) {
    repo.state = RepoState::OK;
  } else {
    repo.state = RepoState::FAILED;
    this->refuse_("Repository %s: index not loaded", repo.name);
  }
  this->refresh_repo_++;
  this->defer([this]() { this->refresh_next_(); });
}

void PackageManager::free_reader_() {
  if (this->reader_ == nullptr)
    return;
  if (this->reader_->inflate != nullptr)
    RAMAllocator<IndexInflate>(RAMAllocator<IndexInflate>::ALLOC_EXTERNAL).deallocate(this->reader_->inflate, 1);
  if (this->reader_->signature != nullptr) {
    this->reader_->signature->~IndexSignature();
    RAMAllocator<IndexSignature>(RAMAllocator<IndexSignature>::ALLOC_EXTERNAL).deallocate(this->reader_->signature, 1);
  }
  RAMAllocator<IndexReader>(RAMAllocator<IndexReader>::ALLOC_EXTERNAL).deallocate(this->reader_, 1);
  this->reader_ = nullptr;
}

static bool copy_whole(char *dst, size_t len, const char *src) {
  if (std::strlen(src) >= len) {
    dst[0] = '\0';
    return false;
  }
  std::strcpy(dst, src);
  return true;
}

static void index_field(AvailablePackage &e, bool &arch_ok, char *line) {
  char *value = std::strchr(line, ':');
  if (value == nullptr)
    return;
  *value++ = '\0';
  while (*value == ' ')
    value++;
  if (std::strcmp(line, "Package") == 0) {
    if (copy_whole(e.name, sizeof(e.name), value)) {
      for (char *c = e.name; *c != '\0'; c++)
        *c = *c == '-' ? '_' : *c;
    }
  } else if (std::strcmp(line, "Version") == 0) {
    copy_whole(e.version, sizeof(e.version), value);
  } else if (std::strcmp(line, "Architecture") == 0) {
    arch_ok = std::strcmp(value, CONFIG_IDF_TARGET) == 0;
  } else if (std::strcmp(line, "Depends") == 0) {
    size_t j = 0;
    for (; *value != '\0' && j + 1 < sizeof(e.depends); value++) {
      if (*value != ' ')
        e.depends[j++] = *value == '-' ? '_' : *value;
    }
    e.depends[j] = '\0';
  } else if (std::strcmp(line, "Filename") == 0) {
    copy_whole(e.filename, sizeof(e.filename), value);
  } else if (std::strcmp(line, "Size") == 0) {
    e.size = strtoul(value, nullptr, 10);
  } else if (std::strcmp(line, "SHA256sum") == 0) {
    e.has_sha256 = std::strlen(value) == 2 * sizeof(e.sha256) && parse_hex(value, e.sha256, sizeof(e.sha256)) ? 1 : 0;
  } else if (std::strcmp(line, "Description") == 0) {
    copy_field(e.description, sizeof(e.description), value);
  } else if (std::strcmp(line, "ESPHome-ABI") == 0) {
    e.abi = strtoul(value, nullptr, 10);
  } else if (std::strcmp(line, "ESPHome-Flash") == 0) {
    e.flash = strtoul(value, nullptr, 10);
  } else if (std::strcmp(line, "ESPHome-PSRAM") == 0) {
    e.psram = strtoul(value, nullptr, 10);
  }
}

void PackageManager::index_line_() {
  IndexReader *r = this->reader_;
  size_t len = r->line_len;
  r->line_len = 0;
  if (len > 0 && r->line[len - 1] == '\r')
    len--;
  r->line[len] = '\0';
  if (len == 0) {
    this->end_paragraph_(r->arch_ok, this->refresh_repo_);
    r->arch_ok = false;
    return;
  }
  if (r->line[0] == ' ' || r->line[0] == '\t')
    return;
  AvailablePackage *slot = this->index_slot_();
  if (slot != nullptr)
    index_field(*slot, r->arch_ok, r->line);
}

AvailablePackage *PackageManager::index_slot_() {
  if (this->available_count_ < this->available_cap_)
    return &this->available_[this->available_count_];
  const size_t cap = this->available_cap_ + AVAILABLE_STEP;
  AvailablePackage *grown = RAMAllocator<AvailablePackage>(RAMAllocator<AvailablePackage>::ALLOC_EXTERNAL)
                                .reallocate(this->available_, cap);
  if (grown == nullptr)
    return nullptr;
  for (size_t i = this->available_cap_; i < cap; i++)
    grown[i] = AvailablePackage{};
  this->available_ = grown;
  this->available_cap_ = static_cast<uint16_t>(cap);
  return &this->available_[this->available_count_];
}

void PackageManager::end_paragraph_(bool arch_ok, uint8_t repo) {
  if (this->available_count_ >= this->available_cap_)
    return;
  AvailablePackage &e = this->available_[this->available_count_];
  if (arch_ok && e.name[0] != '\0' && e.filename[0] != '\0' && this->find_available_(e.name) == nullptr) {
    e.repo = repo;
    this->available_count_++;
    this->repos_[repo].count++;
    if (this->available_count_ < this->available_cap_)
      this->available_[this->available_count_] = AvailablePackage{};
    return;
  }
  e = AvailablePackage{};
}

void PackageManager::refresh_done_() {
  this->free_reader_();
  this->refreshing_ = false;
  ESP_LOGI(TAG, "Repositories refreshed: %u packages", static_cast<unsigned>(this->available_count_));
  this->keep_available_();
  this->changed_();
}

AvailablePackage *PackageManager::find_available_(const char *name) {
  for (size_t i = 0; this->available_ != nullptr && i < this->available_count_; i++) {
    if (std::strncmp(this->available_[i].name, name, sizeof(AvailablePackage::name)) == 0)
      return &this->available_[i];
  }
  return nullptr;
}

void PackageManager::drop_available_(uint8_t repo) {
  if (this->available_ == nullptr)
    return;
  size_t kept = 0;
  for (size_t i = 0; i < this->available_count_; i++) {
    if (this->available_[i].repo != repo)
      this->available_[kept++] = this->available_[i];
  }
  this->available_count_ = static_cast<uint16_t>(kept);
}

void PackageManager::keep_available_() {
  if (this->available_ == nullptr || this->refreshing_ || this->queue_count_ > 0)
    return;
  this->set_timeout(AVAILABLE_TIMEOUT, AVAILABLE_KEEP_MS, [this]() { this->release_available_(); });
}

void PackageManager::release_available_() {
  if (this->available_ == nullptr || this->refreshing_ || this->queue_count_ > 0)
    return;
  RAMAllocator<AvailablePackage>(RAMAllocator<AvailablePackage>::ALLOC_EXTERNAL).deallocate(this->available_,
                                                                                              this->available_cap_);
  this->available_ = nullptr;
  this->available_count_ = 0;
  this->available_cap_ = 0;
}

bool PackageManager::queued_(const char *name) {
  for (size_t i = 0; i < this->queue_count_; i++) {
    if (std::strncmp(this->queue_[i], name, sizeof(this->queue_[i])) == 0)
      return true;
  }
  return false;
}

bool PackageManager::queue_add_(const char *name, uint8_t depth) {
  if (depth >= REPO_QUEUE_MAX)
    return this->refuse_("%s: dependency loop", name);
  const AvailablePackage *pkg = this->find_available_(name);
  if (pkg == nullptr)
    return this->refuse_("%s is not in the repositories", name);
  if (!each_depend(pkg->depends, [this, name, depth](const char *dep) {
        if (this->find_(dep) != nullptr || this->queued_(dep))
          return true;
        if (this->find_available_(dep) == nullptr)
          return this->refuse_("%s needs %s, which is not in the repositories", name, dep);
        return this->queue_add_(dep, depth + 1);
      }))
    return false;
  if (this->queue_count_ >= REPO_QUEUE_MAX)
    return this->refuse_("%s: more than %u packages to install", name, static_cast<unsigned>(REPO_QUEUE_MAX));
  copy_field(this->queue_[this->queue_count_++], sizeof(this->queue_[0]), name);
  return true;
}

bool PackageManager::install_from_repo(const std::string &name) {
  this->last_error_[0] = '\0';
  if (this->installing_ || this->queue_count_ > 0)
    return this->refuse_("%s: not installed, %s is still being installed", name.c_str(), this->pending_name_);
  if (this->refreshing_)
    return this->refuse_("%s: not installed, the repositories are being refreshed", name.c_str());
  if (this->available_ == nullptr)
    return this->refuse_("%s: no repository index loaded, refresh first", name.c_str());
  if (name.empty() || name.size() >= sizeof(AvailablePackage::name))
    return this->refuse_("Install refused: invalid package name '%s'", name.c_str());
  if (!this->open_())
    return false;
  using QueueName = char[sizeof(StoreEntry::name)];
  this->queue_ = RAMAllocator<QueueName>(RAMAllocator<QueueName>::ALLOC_EXTERNAL).allocate(REPO_QUEUE_MAX);
  if (this->queue_ == nullptr)
    return this->refuse_("%s: no PSRAM for the install queue", name.c_str());
  if (!this->queue_add_(name.c_str(), 0)) {
    this->queue_end_();
    return false;
  }
  this->queue_pos_ = 0;
  this->cancel_timeout(AVAILABLE_TIMEOUT);
  return this->queue_next_();
}

bool PackageManager::download(const std::string &name, const std::string &folder) {
  this->last_error_[0] = '\0';
  if (this->installing_ || this->queue_count_ > 0)
    return this->refuse_("%s: not downloaded, %s is being installed", name.c_str(), this->pending_name_);
  if (this->refreshing_)
    return this->refuse_("%s: not downloaded, the repositories are being refreshed", name.c_str());
  if (folder.empty() || folder[0] != '/')
    return this->refuse_("%s: download refused, '%s' is not an absolute folder", name.c_str(), folder.c_str());
  const AvailablePackage *pkg = this->find_available_(name.c_str());
  if (pkg == nullptr)
    return this->refuse_("%s is not in the repositories", name.c_str());
  char rel[REL_LEN];
  storage::PathStorage *dst = this->storage_at_(folder.c_str(), rel, sizeof(rel), pkg->filename);
  const int n = snprintf(this->pending_file_, sizeof(this->pending_file_), "%s/%s", folder.c_str(), pkg->filename);
  if (dst == nullptr || n <= 0 || static_cast<size_t>(n) >= sizeof(this->pending_file_))
    return this->refuse_("%s: no storage for %s", name.c_str(), folder.c_str());
  char build[12];
  snprintf(build, sizeof(build), "/%08" PRIx32 "/", core_abi_id());
  std::string url = this->repos_[pkg->repo].url;
  url += build;
  url += pkg->filename;
  copy_field(this->pending_name_, sizeof(this->pending_name_), name.c_str());
  StorageError err = this->download_(url, dst, rel, pkg->has_sha256 ? pkg->sha256 : nullptr);
  if (err != StorageError::STORAGE_ERROR_OK)
    return this->refuse_("%s: download not started: %s", name.c_str(), storage::error_to_string(err));
  this->download_only_ = true;
  this->installing_ = true;
  this->changed_();
  return true;
}

bool PackageManager::queue_next_() {
  if (this->queue_pos_ >= this->queue_count_) {
    this->queue_end_();
    return true;
  }
  const char *name = this->queue_[this->queue_pos_++];
  const AvailablePackage *pkg = this->find_available_(name);
  if (pkg == nullptr) {
    this->refuse_("%s is not in the repositories", name);
    this->queue_end_();
    return false;
  }
  char build[12];
  snprintf(build, sizeof(build), "/%08" PRIx32 "/", core_abi_id());
  std::string url = this->repos_[pkg->repo].url;
  url += build;
  url += pkg->filename;
  if (!this->install_(url, name, pkg->version, pkg->abi, SettingsTarget::PREFERENCES, "",
                      pkg->has_sha256 ? pkg->sha256 : nullptr)) {
    this->queue_end_();
    return false;
  }
  return true;
}

int PackageManager::download_progress() const {
  if (!this->installing_ || this->container_ == nullptr || this->container_->content_length == 0)
    return -1;
  return static_cast<int>(static_cast<uint64_t>(this->container_->get_bytes_read()) * 100 /
                          this->container_->content_length);
}

bool PackageManager::cancel_install() {
  this->last_error_[0] = '\0';
  if (!this->installing_ && this->queue_count_ == 0)
    return this->refuse_("Nothing is being installed");
  this->queue_count_ = this->queue_pos_;
  if (this->container_ == nullptr || this->dl_ == nullptr)
    return this->refuse_("%s: being written, it finishes", this->pending_name_);
  this->cancel_ = true;
  return true;
}

void PackageManager::queue_end_() {
  this->queue_count_ = 0;
  this->queue_pos_ = 0;
  if (this->queue_ != nullptr) {
    using QueueName = char[sizeof(StoreEntry::name)];
    RAMAllocator<QueueName>(RAMAllocator<QueueName>::ALLOC_EXTERNAL).deallocate(this->queue_, REPO_QUEUE_MAX);
    this->queue_ = nullptr;
  }
  this->keep_available_();
}
#endif

static void settings_namespace(const Package &pkg, char *out, size_t len) {
  snprintf(out, len, "pk%08" PRIx32, fnv1_hash(pkg.name));
}

void PackageManager::files_save_(const Package &pkg, const char *files) {
  if (pkg.settings == SettingsTarget::PREFERENCES) {
    char ns[NVS_NS_NAME_MAX_SIZE];
    settings_namespace(pkg, ns, sizeof(ns));
    nvs_handle_t handle = 0;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK)
      return;
    if (nvs_set_str(handle, FILES_KEY, files) == ESP_OK)
      nvs_commit(handle);
    nvs_close(handle);
    return;
  }
  if (this->directory_ == nullptr)
    return;
  char file[sizeof(StoreEntry::name) + 6];
  snprintf(file, sizeof(file), "%s.files", pkg.name);
  char rel[REL_LEN];
  storage::PathStorage *ps = this->storage_at_(this->directory_, rel, sizeof(rel), file);
  if (ps != nullptr)
    storage::write_file(ps, rel, reinterpret_cast<const uint8_t *>(files), std::strlen(files));
}

void PackageManager::files_erase_(const Package &pkg) {
  if (pkg.settings == SettingsTarget::PREFERENCES) {
    char ns[NVS_NS_NAME_MAX_SIZE];
    settings_namespace(pkg, ns, sizeof(ns));
    nvs_handle_t handle = 0;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK)
      return;
    if (nvs_erase_key(handle, FILES_KEY) == ESP_OK)
      nvs_commit(handle);
    nvs_close(handle);
    return;
  }
  if (this->directory_ == nullptr)
    return;
  char file[sizeof(StoreEntry::name) + 6];
  snprintf(file, sizeof(file), "%s.files", pkg.name);
  char rel[REL_LEN];
  storage::PathStorage *ps = this->storage_at_(this->directory_, rel, sizeof(rel), file);
  if (ps != nullptr)
    ps->remove(rel);
}

#ifdef USE_JSON
void PackageManager::dump_files(const std::string &name, JsonObject root) {
  root["name"] = name;
  JsonArray list = root["files"].to<JsonArray>();
  const Package *pkg = this->find_(name.c_str());
  root["installed"] = pkg != nullptr;
  if (pkg == nullptr)
    return;
  auto add_lines = [&list](const char *text, size_t len) {
    size_t pos = 0;
    while (pos < len) {
      const char *nl = static_cast<const char *>(std::memchr(text + pos, '\n', len - pos));
      const size_t end = nl != nullptr ? static_cast<size_t>(nl - text) : len;
      if (end > pos)
        list.add(std::string(text + pos, end - pos));
      pos = end + 1;
    }
  };
  if (pkg->settings == SettingsTarget::PREFERENCES) {
    char ns[NVS_NS_NAME_MAX_SIZE];
    settings_namespace(*pkg, ns, sizeof(ns));
    nvs_handle_t handle = 0;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK)
      return;
    RAMAllocator<char> chars(RAMAllocator<char>::ALLOC_EXTERNAL);
    char *text = chars.allocate(FILES_MAX);
    size_t size = FILES_MAX;
    if (text != nullptr && nvs_get_str(handle, FILES_KEY, text, &size) == ESP_OK && size > 0)
      add_lines(text, size - 1);
    nvs_close(handle);
    if (text != nullptr)
      chars.deallocate(text, FILES_MAX);
    return;
  }
  if (this->directory_ == nullptr)
    return;
  char file[sizeof(StoreEntry::name) + 6];
  snprintf(file, sizeof(file), "%s.files", pkg->name);
  char rel[REL_LEN];
  storage::PathStorage *ps = this->storage_at_(this->directory_, rel, sizeof(rel), file);
  storage::RamBuffer data;
  size_t size = 0;
  if (ps != nullptr && storage::read_file(ps, rel, data, &size) == StorageError::STORAGE_ERROR_OK)
    add_lines(reinterpret_cast<const char *>(data.get()), size);
}
#endif

int PackageManager::setting_get(const Package &pkg, const char *key, char *out, size_t len) {
  if (key == nullptr || key[0] == '\0' || std::strlen(key) > SETTING_KEY_MAX || out == nullptr || len == 0)
    return -1;
  if (pkg.settings == SettingsTarget::PREFERENCES) {
    char ns[NVS_NS_NAME_MAX_SIZE];
    settings_namespace(pkg, ns, sizeof(ns));
    nvs_handle_t handle = 0;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK)
      return -1;
    size_t size = len;
    esp_err_t err = nvs_get_str(handle, key, out, &size);
    nvs_close(handle);
    return err == ESP_OK ? static_cast<int>(size > 0 ? size - 1 : 0) : -1;
  }
  if (this->directory_ == nullptr)
    return -1;
  char file[sizeof(StoreEntry::name) + 4];
  snprintf(file, sizeof(file), "%s.cfg", pkg.name);
  char rel[REL_LEN];
  storage::PathStorage *ps = this->storage_at_(this->directory_, rel, sizeof(rel), file);
  storage::RamBuffer data;
  size_t size = 0;
  if (ps == nullptr || storage::read_file(ps, rel, data, &size) != StorageError::STORAGE_ERROR_OK)
    return -1;
  const size_t key_len = std::strlen(key);
  size_t pos = 0;
  while (pos < size) {
    const uint8_t *nl = static_cast<const uint8_t *>(std::memchr(data.get() + pos, '\n', size - pos));
    size_t end = nl != nullptr ? static_cast<size_t>(nl - data.get()) : size;
    if (end - pos > key_len && data.get()[pos + key_len] == '=' && std::memcmp(data.get() + pos, key, key_len) == 0) {
      size_t n = end - pos - key_len - 1;
      if (n >= len)
        return -1;
      std::memcpy(out, data.get() + pos + key_len + 1, n);
      out[n] = '\0';
      return static_cast<int>(n);
    }
    pos = end + 1;
  }
  return -1;
}

int PackageManager::setting_set(const Package &pkg, const char *key, const char *value) {
  if (key == nullptr || key[0] == '\0' || std::strlen(key) > SETTING_KEY_MAX || std::strchr(key, '=') != nullptr ||
      value == nullptr || std::strlen(value) > SETTING_VALUE_MAX || std::strchr(value, '\n') != nullptr)
    return -1;
  if (pkg.settings == SettingsTarget::PREFERENCES) {
    char ns[NVS_NS_NAME_MAX_SIZE];
    settings_namespace(pkg, ns, sizeof(ns));
    nvs_handle_t handle = 0;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK)
      return -1;
    esp_err_t err = nvs_set_str(handle, key, value);
    if (err == ESP_OK)
      err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK ? 0 : -1;
  }
  if (this->directory_ == nullptr)
    return -1;
  char file[sizeof(StoreEntry::name) + 4];
  snprintf(file, sizeof(file), "%s.cfg", pkg.name);
  char tmp_file[sizeof(StoreEntry::name) + 8];
  snprintf(tmp_file, sizeof(tmp_file), "%s.cfg.tmp", pkg.name);
  char rel[REL_LEN];
  char tmp[REL_LEN];
  storage::PathStorage *ps = this->storage_at_(this->directory_, rel, sizeof(rel), file);
  if (ps == nullptr || this->storage_at_(this->directory_, tmp, sizeof(tmp), tmp_file) == nullptr)
    return -1;
  storage::RamBuffer data;
  size_t size = 0;
  StorageError err = storage::read_file(ps, rel, data, &size);
  if (err != StorageError::STORAGE_ERROR_OK && err != StorageError::STORAGE_ERROR_NOT_FOUND)
    return -1;
  if (err == StorageError::STORAGE_ERROR_NOT_FOUND)
    size = 0;
  const size_t key_len = std::strlen(key);
  const size_t value_len = std::strlen(value);
  const size_t cap = size + key_len + value_len + 3;
  RAMAllocator<uint8_t> allocator;
  uint8_t *text = allocator.allocate(cap);
  if (text == nullptr)
    return -1;
  size_t len = 0;
  size_t pos = 0;
  while (pos < size) {
    const uint8_t *nl = static_cast<const uint8_t *>(std::memchr(data.get() + pos, '\n', size - pos));
    size_t end = nl != nullptr ? static_cast<size_t>(nl - data.get()) : size;
    bool same = end - pos > key_len && data.get()[pos + key_len] == '=' &&
                std::memcmp(data.get() + pos, key, key_len) == 0;
    if (!same && end > pos) {
      std::memcpy(text + len, data.get() + pos, end - pos);
      len += end - pos;
      text[len++] = '\n';
    }
    pos = end + 1;
  }
  std::memcpy(text + len, key, key_len);
  len += key_len;
  text[len++] = '=';
  std::memcpy(text + len, value, value_len);
  len += value_len;
  text[len++] = '\n';
  err = storage::write_file(ps, tmp, text, len);
  allocator.deallocate(text, cap);
  if (err == StorageError::STORAGE_ERROR_OK) {
    ps->remove(rel);
    err = ps->rename(tmp, rel);
  }
  if (err != StorageError::STORAGE_ERROR_OK) {
    ps->remove(tmp);
    return -1;
  }
  return 0;
}

}

#endif
