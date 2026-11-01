#include "nvs_manager.h"

#if defined(USE_ESP32) && defined(USE_ESP32_NVS_GUARD)

#include "esphome/components/esp32/nvs_browse.h"
#include "esphome/components/esp32/nvs_guard.h"
#include "esphome/components/esp32/preferences.h"
#include "esphome/core/application.h"
#include "esphome/core/build_info_data.h"
#include "esphome/core/log.h"
#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <esp_err.h>

namespace esphome::nvs {

static const char *const TAG = "nvs";

namespace guard = esp32::nvs_guard;

void NvsManager::setup() {
  this->defer([this]() { this->after_setup_(); });
}

void NvsManager::dump_config() {
  const auto &s = guard::state();
  ESP_LOGCONFIG(TAG,
                "NVS:\n"
                "  Boot check: %s\n"
                "  Policy: %s%s",
                guard::result_name(s.result), guard::policy_name(s.applied), s.requested ? " (requested)" : "");
}

void NvsManager::after_setup_() {
  const auto &s = guard::state();
  if (s.sweep_pending)
    guard::sweep(esp32::get_preferences()->nvs_handle);
  if (s.result == guard::Result::CORRUPT) {
    ESP_LOGW(TAG, "Partition unreadable (%s): %s", esp_err_to_name(s.init_err), guard::policy_name(s.applied));
  } else if (s.result != guard::Result::SAME || s.applied != guard::Policy::KEEP) {
    ESP_LOGI(TAG, "%s: %s%s, %u restored, %u invalidated", guard::result_name(s.result), guard::policy_name(s.applied),
             s.requested ? " (requested)" : "", s.restored, s.invalidated);
  }
#if defined(USE_STORAGE) && defined(ESP32_NVS_BACKUP_DIR)
  if (s.backup != nullptr) {
    char path[160];
    snprintf(path, sizeof(path), "%s/nvs-%s-%08" PRIx32 ".bin", ESP32_NVS_BACKUP_DIR, guard::result_name(s.result),
             static_cast<uint32_t>(ESPHOME_CONFIG_HASH));
    this->backup(path);
  }
#endif
  if (s.backup != nullptr && !esp_ptr_external_ram(s.backup))
    guard::release_backup();
  this->check();
}

void NvsManager::publish_(size_t entries, uint16_t unreadable) {
  const auto &s = guard::state();
  char text[96];
  snprintf(text, sizeof(text), "%s, %s, %u entries%s", guard::result_name(s.result), guard::policy_name(s.applied),
           static_cast<unsigned>(entries), s.read_only ? ", read only" : "");
  if (unreadable != 0) {
    const size_t len = strlen(text);
    snprintf(text + len, sizeof(text) - len, ", %u unreadable", unreadable);
  }
#ifdef USE_TEXT_SENSOR
  if (this->state_ != nullptr)
    this->state_->publish_state(text);
#endif
  this->check_trigger_.trigger(guard::result_name(s.result), text);
}

bool NvsManager::check() {
  if (!esp32::get_preferences()->nvs_ready) {
    this->publish_(0, 0);
    return false;
  }
  uint16_t unreadable = 0;
  const size_t entries = guard::verify(&unreadable);
  if (unreadable != 0)
    ESP_LOGW(TAG, "%u of %u entries unreadable", unreadable, static_cast<unsigned>(entries));
  this->publish_(entries, unreadable);
  return unreadable == 0;
}

bool NvsManager::invalidate(const std::string &ns, const std::string &key) {
  if (ns.empty() || ns.size() >= NVS_NS_NAME_MAX_SIZE || key.size() >= NVS_KEY_NAME_MAX_SIZE)
    return false;
  const bool ok = esp32::nvs_browse::erase(ns.c_str(), key.c_str());
  ESP_LOGI(TAG, "Invalidate %s%s%s: %s", ns.c_str(), key.empty() ? "" : "/", key.c_str(), ok ? "done" : "failed");
  return ok;
}

bool NvsManager::reset(bool keep_protected, bool reboot) {
  uint16_t restored = 0;
  const bool ok = guard::reset(keep_protected, &restored);
  ESP_LOGI(TAG, "Reset%s: %s, %u restored", keep_protected ? " (protected kept)" : "", ok ? "done" : "failed",
           restored);
  if (!esp_ptr_external_ram(guard::state().backup))
    guard::release_backup();
  if (ok && reboot)
    App.safe_reboot();
  return ok;
}

bool NvsManager::request(const std::string &policy) {
  guard::Policy p;
  if (!guard::parse_policy(policy.c_str(), &p))
    return false;
  const bool ok = guard::request(p);
  ESP_LOGI(TAG, "Next boot: %s%s", policy.c_str(), ok ? "" : " (not stored)");
  return ok;
}

#ifdef USE_STORAGE
static storage::PathStorage *resolve(const std::string &path, const char **rel) {
  if (storage::global_storage_registry == nullptr)
    return nullptr;
  auto *ps = storage::global_storage_registry->resolve_path(path.c_str(), rel);
  return ps != nullptr && *rel != nullptr && (*rel)[0] != '\0' ? ps : nullptr;
}

bool NvsManager::backup(const std::string &path) {
  const char *rel = nullptr;
  auto *ps = resolve(path, &rel);
  if (ps == nullptr) {
    ESP_LOGW(TAG, "Backup: %s is no file on a mounted storage", path.c_str());
    return false;
  }
  const auto &s = guard::state();
  const bool held = s.backup != nullptr;
  uint8_t *data = s.backup;
  size_t size = s.backup_size;
  if (!held)
    size = guard::snapshot(false, &data);
  const bool ok = data != nullptr && storage::write_file(ps, rel, data, size) == storage::StorageError::STORAGE_ERROR_OK;
  if (held) {
    if (ok)
      guard::release_backup();
  } else {
    heap_caps_free(data);
  }
  ESP_LOGI(TAG, "Backup %s: %s", path.c_str(), ok ? "written" : "failed");
  return ok;
}

bool NvsManager::restore(const std::string &path, bool protected_only, bool reboot) {
  const char *rel = nullptr;
  auto *ps = resolve(path, &rel);
  if (ps == nullptr) {
    ESP_LOGW(TAG, "Restore: %s is no file on a mounted storage", path.c_str());
    return false;
  }
  storage::RamBuffer data;
  size_t size = 0;
  uint16_t count = 0;
  const bool ok = storage::read_file(ps, rel, data, &size) == storage::StorageError::STORAGE_ERROR_OK &&
                  guard::restore(data.get(), size, protected_only, &count);
  data.reset();
  ESP_LOGI(TAG, "Restore %s: %s, %u entries", path.c_str(), ok ? "done" : "failed", count);
  if (ok && reboot)
    App.safe_reboot();
  return ok;
}
#endif

}

#endif
