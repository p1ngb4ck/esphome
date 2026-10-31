#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && (defined(USE_PREFERENCES_BACKUP) || defined(USE_ESP32_PREFERENCES_STORAGE))

namespace esphome {
namespace storage {
class KeyValueStorage;
}
}  // namespace esphome

namespace esphome::preferences {

// Bind the external store that flash preferences are routed through. Recorded here and, on a
// build with the esp32 redirect compiled in, handed on to that backend. Called from codegen.
void set_external_store(storage::KeyValueStorage *kv);

// The store the node's preferences actually live in, as a KeyValueStorage view: the external
// store when one is bound, otherwise a view on the internal "esphome" NVS namespace. This is
// what whole-namespace enumeration (preferences backup/restore) works on. Null only when the
// internal namespace cannot be opened.
storage::KeyValueStorage *get_backup_store();

}  // namespace esphome::preferences

#endif
