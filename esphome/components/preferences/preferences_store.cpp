#include "preferences_store.h"

#if defined(USE_ESP32) && (defined(USE_PREFERENCES_BACKUP) || defined(USE_ESP32_PREFERENCES_STORAGE))

#include "esphome/components/storage/storage.h"
#ifdef USE_PREFERENCES_BACKUP
#include "esphome/components/binary_storage/nvs_store.h"
#endif
#ifdef USE_ESP32_PREFERENCES_STORAGE
#include "esphome/components/esp32/preferences.h"
#endif

namespace esphome::preferences {

static storage::KeyValueStorage *s_external = nullptr;  // NOLINT

void set_external_store(storage::KeyValueStorage *kv) {
  s_external = kv;
#ifdef USE_ESP32_PREFERENCES_STORAGE
  // The esp32 backend owns the get/set redirect for flash preferences; it only needs to be told
  // which store to use. Keeping the pointer here as well is what lets the backup engine follow
  // the same store without reaching into that backend.
  esp32::set_external_preferences_store(kv);
#endif
}

storage::KeyValueStorage *get_backup_store() {
#ifdef USE_PREFERENCES_BACKUP
  if (s_external != nullptr)
    return s_external;
  // No external store: open our own view on the system "esphome" namespace. NVS allows several
  // handles on one namespace, so this coexists with the handle ESP32Preferences::open() holds --
  // no adopt_handle() and therefore no dependency on that backend. The handle is opened lazily on
  // first access, long after nvs_flash_init() has succeeded, so NVSStore's format-on-empty path
  // cannot trigger here.
  static binary_storage::NVSStore s_internal;  // NOLINT
  static bool s_named = false;
  if (!s_named) {
    s_internal.set_namespace("esphome");
    s_named = true;
  }
  return &s_internal;
#else
  return s_external;
#endif
}

}  // namespace esphome::preferences

#endif
