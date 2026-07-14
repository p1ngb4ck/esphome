#pragma once

#include "esphome/core/defines.h"
// Preferences backup/restore is an ESP32-only storage capability: it talks to
// the NVS namespace ESPHome preferences live in. Codegen defines
// USE_STORAGE_PREFERENCES when one of the actions is used.
#if defined(USE_STORAGE_PREFERENCES) && defined(USE_ESP32)

#include <cstddef>
#include <cstdint>

namespace esphome {
class EntityBase;  // fwd — full definition pulled in by the .cpp only
}  // namespace esphome

namespace esphome::storage {

// Value type of a selected preference, baked by codegen from the global's
// YAML `type:`. Blobs are the raw bytes of T (strings: length-prefixed
// char[SZ], see globals_component.h) — the tag is what makes them readable.
enum class PrefType : uint8_t {
  HEX = 0,  // unknown/unsupported type — hex round-trip fallback
  BOOL,
  I8,
  U8,
  I16,
  U16,
  I32,
  U32,
  F32,
  F64,
  STRING,  // length-prefixed char[count] (count == SZ)
};

struct PrefSelection {
  const char *name;
  uint32_t key;
  PrefType type;
  // scalar: 1; array T[N]: N; STRING: SZ (buffer size incl. length byte)
  uint16_t count;
};

// The selection table provides names and types. Codegen ALWAYS bakes it: from
// the action's `preferences:` list when given (restrict == true: only those
// entries round-trip), otherwise from every restore_value global in the
// config (restrict == false: the whole namespace round-trips, table entries
// render readable, unknown keys — entity states etc. — fall back to hex).
bool preferences_export_to_storage(const char *path, const char *format, const PrefSelection *selection,
                                   size_t count, bool restrict_to_selection, const char *const *entity_names,
                                   size_t entity_name_count);
bool preferences_import_from_storage(const char *path, const char *format, bool reboot,
                                     const PrefSelection *selection, size_t count, bool restrict_to_selection,
                                     const char *const *entity_names, size_t entity_name_count);

// ---- runtime entity name registry (baked registration calls) ----
// Codegen enumerates entity IDs in its coroutine and emits one registration
// call per restoring entity into setup(); the KEY comes from the entity
// object itself at runtime (get_preference_hash() ^ per-type version), so no
// hash recipe is replicated at codegen time. Values stay hex (component-
// private restore structs); this registry provides the NAMES.
// What the entity's restore blob IS — codegen picks this from the declared
// class; the codecs in preferences_backup.cpp are compiled against the REAL
// component structs (sizeof-gated, hex fallback on mismatch), so no layout
// knowledge is duplicated here.
enum class EntityKind : uint8_t {
  RAW = 0,  // unknown restore layout: named, hex value
  BOOL,     // switch & friends
  FLOAT,    // number, integration, sprinkler, ...
  STRING,   // text (length-prefixed; aux = SZ incl. length byte)
  FAN,
  COVER,
  VALVE,
  LIGHT,
  CLIMATE,
  SELECT_INDEX,  // template select restores a size_t option index
  MEDIA_VOLUME,  // media players restore {float volume; bool is_muted}
  DATE,
  TIME,
  DATETIME,
};

void register_entity_pref(esphome::EntityBase *entity, const char *name, uint32_t version, EntityKind kind,
                          uint16_t aux = 0);
#ifdef USE_TEXT
namespace detail {
void register_text_pref_impl(esphome::EntityBase *entity, const char *name, uint32_t min_len, uint32_t max_len,
                             const char *pattern);
}  // namespace detail
#endif  // USE_TEXT

}  // namespace esphome::storage

#endif  // USE_STORAGE_PREFERENCES && USE_ESP32
