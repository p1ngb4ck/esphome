#include "preferences_backup.h"

#if defined(USE_STORAGE_PREFERENCES) && defined(USE_ESP32)

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "storage.h"

#include <vector>

#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"

// Codecs compile against the REAL component restore structs — field access by
// name, layouts stay the compiler's problem, sizeof gates every decode.
#ifdef USE_FAN
#include "esphome/components/fan/fan.h"
#endif
#ifdef USE_COVER
#include "esphome/components/cover/cover.h"
#endif
#ifdef USE_VALVE
#include "esphome/components/valve/valve.h"
#endif
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif
#ifdef USE_DATETIME_DATE
#include "esphome/components/datetime/date_entity.h"
#endif
#ifdef USE_DATETIME_TIME
#include "esphome/components/datetime/time_entity.h"
#endif
#ifdef USE_DATETIME_DATETIME
#include "esphome/components/datetime/datetime_entity.h"
#endif

#include "esphome/components/json/json_util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <nvs.h>

namespace esphome::storage {

static const char *const TAG = "storage.preferences";

// The namespace every ESPHome preference lives in (see esp32/preferences.cpp).
static constexpr const char *NVS_NAMESPACE = "esphome";
static constexpr size_t MAX_BLOB_LEN = 4096;  // NVS blob hard limit is well below this
static constexpr const char *HEX_PREFIX = "hex:";

struct RuntimeEntry {
  uint32_t key;
  const char *name;
  EntityKind kind;
  uint16_t aux;  // STRING: SZ incl. length byte
};

static std::vector<RuntimeEntry> &runtime_registry() {
  static std::vector<RuntimeEntry> reg;  // function-local: safe init order
  return reg;
}

void register_entity_pref(esphome::EntityBase *entity, const char *name, uint32_t version, EntityKind kind,
                          uint16_t aux) {
  // Central recipe from EntityBase::make_entity_preference_() — the entity
  // supplies its own hash; only the per-type version constant is baked.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  const uint32_t key = entity->get_preference_hash() ^ version;
#pragma GCC diagnostic pop
  runtime_registry().push_back({key, name, kind, aux});
}

#ifdef USE_TEXT
namespace detail {
void register_text_pref_impl(esphome::EntityBase *entity, const char *name, uint32_t min_len, uint32_t max_len,
                             const char *pattern) {
  // template_text does NOT use make_entity_preference: its key adds trait
  // salts on top of the base hash — replicated 1:1 from
  // template/text/template_text.cpp (traits come from the live object via
  // codegen, so the salt inputs are always the real ones).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  uint32_t key = entity->get_preference_hash();
#pragma GCC diagnostic pop
  key += min_len << 2;
  key += max_len << 4;
  key += fnv1_hash(pattern) << 6;
  runtime_registry().push_back({key, name, EntityKind::STRING, static_cast<uint16_t>(max_len + 1)});
}
}  // namespace detail
#endif  // USE_TEXT

static const RuntimeEntry *runtime_by_key(uint32_t key, const char *const *allowed, size_t allowed_count) {
  for (const auto &e : runtime_registry()) {
    if (e.key != key)
      continue;
    if (allowed == nullptr)
      return &e;
    for (size_t i = 0; i < allowed_count; i++) {
      if (strcmp(allowed[i], e.name) == 0)
        return &e;
    }
    return nullptr;  // known, but filtered out by the action's selection
  }
  return nullptr;
}

static const RuntimeEntry *runtime_by_name(const char *token, size_t len, const char *const *allowed,
                                           size_t allowed_count) {
  for (const auto &e : runtime_registry()) {
    if (strlen(e.name) != len || memcmp(e.name, token, len) != 0)
      continue;
    if (allowed == nullptr)
      return &e;
    for (size_t i = 0; i < allowed_count; i++) {
      if (strcmp(allowed[i], e.name) == 0)
        return &e;
    }
    return nullptr;
  }
  return nullptr;
}

static const PrefSelection *find_by_key(uint32_t key, const PrefSelection *sel, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (sel[i].key == key)
      return &sel[i];
  }
  return nullptr;
}

static const PrefSelection *find_by_name(const char *token, size_t len, const PrefSelection *sel, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strlen(sel[i].name) == len && memcmp(sel[i].name, token, len) == 0)
      return &sel[i];
  }
  return nullptr;
}

static size_t scalar_size(PrefType t) {
  switch (t) {
    case PrefType::BOOL:
    case PrefType::I8:
    case PrefType::U8:
      return 1;
    case PrefType::I16:
    case PrefType::U16:
      return 2;
    case PrefType::I32:
    case PrefType::U32:
    case PrefType::F32:
      return 4;
    case PrefType::F64:
      return 8;
    default:
      return 0;
  }
}

static void append_hex(std::string &out, const uint8_t *data, size_t len) {
  static const char *const HEX = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out += HEX[data[i] >> 4];
    out += HEX[data[i] & 0x0F];
  }
}

static bool parse_hex(const char *s, size_t len, uint8_t *out, size_t *out_len) {
  if (len % 2 != 0 || len / 2 > MAX_BLOB_LEN)
    return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; i += 2) {
    int hi = nib(s[i]), lo = nib(s[i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out[i / 2] = (hi << 4) | lo;
  }
  *out_len = len / 2;
  return true;
}

// ---- scalar element <-> text (little-endian raw bytes, ESP32 native) ----

static void encode_scalar(std::string &out, PrefType t, const uint8_t *p) {
  char buf[32];
  switch (t) {
    case PrefType::BOOL:
      out += (*p != 0) ? "true" : "false";
      return;
    case PrefType::I8: {
      int8_t v;
      memcpy(&v, p, 1);
      snprintf(buf, sizeof(buf), "%d", (int) v);
      break;
    }
    case PrefType::U8:
      snprintf(buf, sizeof(buf), "%u", (unsigned) *p);
      break;
    case PrefType::I16: {
      int16_t v;
      memcpy(&v, p, 2);
      snprintf(buf, sizeof(buf), "%d", (int) v);
      break;
    }
    case PrefType::U16: {
      uint16_t v;
      memcpy(&v, p, 2);
      snprintf(buf, sizeof(buf), "%u", (unsigned) v);
      break;
    }
    case PrefType::I32: {
      int32_t v;
      memcpy(&v, p, 4);
      snprintf(buf, sizeof(buf), "%" PRId32, v);
      break;
    }
    case PrefType::U32: {
      uint32_t v;
      memcpy(&v, p, 4);
      snprintf(buf, sizeof(buf), "%" PRIu32, v);
      break;
    }
    case PrefType::F32: {
      float v;
      memcpy(&v, p, 4);
      snprintf(buf, sizeof(buf), "%.9g", (double) v);
      break;
    }
    case PrefType::F64: {
      double v;
      memcpy(&v, p, 8);
      snprintf(buf, sizeof(buf), "%.17g", v);
      break;
    }
    default:
      return;
  }
  out += buf;
}

static bool decode_scalar(const char *s, size_t len, PrefType t, uint8_t *p) {
  char buf[48];
  if (len == 0 || len >= sizeof(buf))
    return false;
  memcpy(buf, s, len);
  buf[len] = '\0';
  char *end = nullptr;
  switch (t) {
    case PrefType::BOOL: {
      bool v;
      if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0) {
        v = true;
      } else if (strcmp(buf, "false") == 0 || strcmp(buf, "0") == 0) {
        v = false;
      } else {
        return false;
      }
      uint8_t b = v ? 1 : 0;
      memcpy(p, &b, 1);
      return true;
    }
    case PrefType::F32: {
      float v = strtof(buf, &end);
      if (end == nullptr || *end != '\0')
        return false;
      memcpy(p, &v, 4);
      return true;
    }
    case PrefType::F64: {
      double v = strtod(buf, &end);
      if (end == nullptr || *end != '\0')
        return false;
      memcpy(p, &v, 8);
      return true;
    }
    default: {
      long long v = strtoll(buf, &end, 10);
      if (end == nullptr || *end != '\0')
        return false;
      int64_t iv = v;
      memcpy(p, &iv, scalar_size(t));  // little-endian truncation = native narrowing
      return true;
    }
  }
}

// ---- whole-value <-> text ----

// Renders a typed blob into `out`. Falls back to "hex:<...>" when the blob
// does not match the expected layout (stale entry from an older config).
static void encode_value(std::string &out, const PrefSelection &s, const uint8_t *blob, size_t len) {
  const size_t es = scalar_size(s.type);
  if (s.type == PrefType::STRING) {
    // length-prefixed char[SZ]: blob[0] = size, then the bytes
    if (len >= 1 && len <= MAX_BLOB_LEN && blob[0] < len) {
      const char *str = reinterpret_cast<const char *>(blob + 1);
      const size_t slen = blob[0];
      // newlines would break the line-based kv format — hex-fall back
      if (memchr(str, '\n', slen) == nullptr && memchr(str, '\r', slen) == nullptr) {
        out.append(str, slen);
        return;
      }
    }
  } else if (es != 0 && len == es * s.count) {
    for (uint16_t i = 0; i < s.count; i++) {
      if (i != 0)
        out += ',';
      encode_scalar(out, s.type, blob + i * es);
    }
    return;
  }
  out += HEX_PREFIX;
  append_hex(out, blob, len);
}

// Parses a typed value text back into a blob. Accepts the "hex:" fallback for
// every type.
static bool decode_value(const char *s, size_t len, const PrefSelection &sel, uint8_t *blob, size_t *blob_len) {
  if (len >= strlen(HEX_PREFIX) && memcmp(s, HEX_PREFIX, strlen(HEX_PREFIX)) == 0) {
    return parse_hex(s + strlen(HEX_PREFIX), len - strlen(HEX_PREFIX), blob, blob_len);
  }
  const size_t es = scalar_size(sel.type);
  if (sel.type == PrefType::STRING) {
    if (sel.count == 0 || len >= sel.count)  // SZ includes the length byte
      return false;
    blob[0] = static_cast<uint8_t>(len);
    memcpy(blob + 1, s, len);
    // globals compare/save the full SZ buffer; zero the tail for determinism
    memset(blob + 1 + len, 0, sel.count - 1 - len);
    *blob_len = sel.count;
    return true;
  }
  if (es == 0)
    return false;
  size_t idx = 0, start = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || s[i] == ',') {
      if (idx >= sel.count)
        return false;
      if (!decode_scalar(s + start, i - start, sel.type, blob + idx * es))
        return false;
      idx++;
      start = i + 1;
    }
  }
  if (idx != sel.count)
    return false;
  *blob_len = es * sel.count;
  return true;
}

// ---- entity value codecs (real component structs) ----

[[maybe_unused]] static void kv_field(std::string &out, const char *k, const char *v, bool &first) {
  if (!first)
    out += ',';
  first = false;
  out += k;
  out += ':';
  out += v;
}
[[maybe_unused]] static void kv_field_f(std::string &out, const char *k, float v, bool &first) {
  char b[24];
  snprintf(b, sizeof(b), "%.9g", (double) v);
  kv_field(out, k, b, first);
}
[[maybe_unused]] static void kv_field_i(std::string &out, const char *k, long v, bool &first) {
  char b[16];
  snprintf(b, sizeof(b), "%ld", v);
  kv_field(out, k, b, first);
}
[[maybe_unused]] static void kv_field_b(std::string &out, const char *k, bool v, bool &first) {
  kv_field(out, k, v ? "true" : "false", first);
}

// Tiny "{k:v,k:v}" reader shared by all struct decoders.
struct FieldReader {
  const char *p;
  const char *end;
  bool ok{true};
  bool get(const char *key, char *val, size_t val_size) {
    // fields may arrive in any order; scan from the start each time
    const char *s = this->p;
    size_t klen = strlen(key);
    while (s < this->end) {
      const char *colon = static_cast<const char *>(memchr(s, ':', this->end - s));
      if (colon == nullptr)
        break;
      const char *comma = static_cast<const char *>(memchr(colon, ',', this->end - colon));
      const char *vend = comma != nullptr ? comma : this->end;
      if (static_cast<size_t>(colon - s) == klen && memcmp(s, key, klen) == 0) {
        size_t vlen = vend - (colon + 1);
        if (vlen >= val_size)
          return false;
        memcpy(val, colon + 1, vlen);
        val[vlen] = '\0';
        return true;
      }
      s = comma != nullptr ? comma + 1 : this->end;
    }
    return false;
  }
  bool f(const char *key, float &out) {
    char b[32];
    if (!this->get(key, b, sizeof(b)))
      return false;
    char *e = nullptr;
    out = strtof(b, &e);
    return e != nullptr && *e == '\0';
  }
  bool i(const char *key, long &out) {
    char b[24];
    if (!this->get(key, b, sizeof(b)))
      return false;
    char *e = nullptr;
    out = strtol(b, &e, 10);
    return e != nullptr && *e == '\0';
  }
  bool b(const char *key, bool &out) {
    char v[8];
    if (!this->get(key, v, sizeof(v)))
      return false;
    if (strcmp(v, "true") == 0 || strcmp(v, "1") == 0) {
      out = true;
      return true;
    }
    if (strcmp(v, "false") == 0 || strcmp(v, "0") == 0) {
      out = false;
      return true;
    }
    return false;
  }
};

// Renders a runtime entry's blob readable; returns false to hex-fall back
// (unknown kind, or blob size does not match the compiled struct — stale).
static bool encode_entity_value(std::string &out, const RuntimeEntry &re, const uint8_t *blob, size_t len) {
  switch (re.kind) {
    case EntityKind::BOOL:
      if (len != sizeof(bool))
        return false;
      out += (*blob != 0) ? "true" : "false";
      return true;
    case EntityKind::FLOAT: {
      if (len != sizeof(float))
        return false;
      float v;
      memcpy(&v, blob, sizeof(v));
      char b[24];
      snprintf(b, sizeof(b), "%.9g", (double) v);
      out += b;
      return true;
    }
    case EntityKind::STRING: {
      // length-prefixed char[SZ]; same layout as string globals
      if (len < 1 || blob[0] >= len)
        return false;
      const char *s = reinterpret_cast<const char *>(blob + 1);
      if (memchr(s, '\n', blob[0]) != nullptr || memchr(s, '\r', blob[0]) != nullptr)
        return false;
      out.append(s, blob[0]);
      return true;
    }
#ifdef USE_FAN
    case EntityKind::FAN: {
      if (len != sizeof(fan::FanRestoreState))
        return false;
      fan::FanRestoreState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_b(out, "state", st.state, first);
      kv_field_i(out, "speed", st.speed, first);
      kv_field_b(out, "oscillating", st.oscillating, first);
      kv_field_i(out, "direction", static_cast<long>(st.direction), first);
      kv_field_i(out, "preset_mode", st.preset_mode, first);
      out += '}';
      return true;
    }
#endif
#ifdef USE_COVER
    case EntityKind::COVER: {
      if (len != sizeof(cover::CoverRestoreState))
        return false;
      cover::CoverRestoreState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_f(out, "position", st.position, first);
      kv_field_f(out, "tilt", st.tilt, first);
      out += '}';
      return true;
    }
#endif
#ifdef USE_VALVE
    case EntityKind::VALVE: {
      if (len != sizeof(valve::ValveRestoreState))
        return false;
      valve::ValveRestoreState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_f(out, "position", st.position, first);
      out += '}';
      return true;
    }
#endif
#ifdef USE_LIGHT
    case EntityKind::LIGHT: {
      if (len != sizeof(light::LightStateRTCState))
        return false;
      light::LightStateRTCState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_b(out, "state", st.state, first);
      kv_field_f(out, "brightness", st.brightness, first);
      kv_field_f(out, "color_brightness", st.color_brightness, first);
      kv_field_f(out, "red", st.red, first);
      kv_field_f(out, "green", st.green, first);
      kv_field_f(out, "blue", st.blue, first);
      kv_field_f(out, "white", st.white, first);
      kv_field_f(out, "color_temp", st.color_temp, first);
      kv_field_f(out, "cold_white", st.cold_white, first);
      kv_field_f(out, "warm_white", st.warm_white, first);
      kv_field_i(out, "effect", st.effect, first);
      kv_field_i(out, "color_mode", static_cast<long>(st.color_mode), first);
      out += '}';
      return true;
    }
#endif
#ifdef USE_CLIMATE
    case EntityKind::CLIMATE: {
      if (len != sizeof(climate::ClimateDeviceRestoreState))
        return false;
      climate::ClimateDeviceRestoreState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_i(out, "mode", static_cast<long>(st.mode), first);
      kv_field_b(out, "uses_custom_fan_mode", st.uses_custom_fan_mode, first);
      kv_field_i(out, "fan_mode", st.uses_custom_fan_mode ? st.custom_fan_mode : static_cast<long>(st.fan_mode),
                 first);
      kv_field_b(out, "uses_custom_preset", st.uses_custom_preset, first);
      kv_field_i(out, "preset", st.uses_custom_preset ? st.custom_preset : static_cast<long>(st.preset), first);
      kv_field_i(out, "swing_mode", static_cast<long>(st.swing_mode), first);
      // two-point control shares the union — export both words, they alias
      kv_field_f(out, "target_temperature_low", st.target_temperature_low, first);
      kv_field_f(out, "target_temperature_high", st.target_temperature_high, first);
      kv_field_f(out, "target_humidity", st.target_humidity, first);
      out += '}';
      return true;
    }
#endif
    case EntityKind::SELECT_INDEX: {
      // template select restores its option index as size_t
      if (len != sizeof(size_t))
        return false;
      size_t v;
      memcpy(&v, blob, sizeof(v));
      char b[16];
      snprintf(b, sizeof(b), "%zu", v);
      out += b;
      return true;
    }
    case EntityKind::MEDIA_VOLUME: {
      // {float volume; bool is_muted} — defined identically (and trivially)
      // in speaker/media_player/speaker_media_player.h and
      // speaker_source/speaker_source_media_player.h; both are platform
      // headers we cannot include generically, so this mirrors the layout
      // with the usual sizeof gate (mismatch -> hex fallback).
      struct MediaVolumeState {
        float volume;
        bool is_muted;
      } st;
      if (len != sizeof(st))
        return false;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_f(out, "volume", st.volume, first);
      kv_field_b(out, "is_muted", st.is_muted, first);
      out += '}';
      return true;
    }
#ifdef USE_DATETIME_DATE
    case EntityKind::DATE: {
      if (len != sizeof(datetime::DateEntityRestoreState))
        return false;
      datetime::DateEntityRestoreState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_i(out, "year", st.year, first);
      kv_field_i(out, "month", st.month, first);
      kv_field_i(out, "day", st.day, first);
      out += '}';
      return true;
    }
#endif
#ifdef USE_DATETIME_TIME
    case EntityKind::TIME: {
      if (len != sizeof(datetime::TimeEntityRestoreState))
        return false;
      datetime::TimeEntityRestoreState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_i(out, "hour", st.hour, first);
      kv_field_i(out, "minute", st.minute, first);
      kv_field_i(out, "second", st.second, first);
      out += '}';
      return true;
    }
#endif
#ifdef USE_DATETIME_DATETIME
    case EntityKind::DATETIME: {
      if (len != sizeof(datetime::DateTimeEntityRestoreState))
        return false;
      datetime::DateTimeEntityRestoreState st;
      memcpy(&st, blob, sizeof(st));
      bool first = true;
      out += '{';
      kv_field_i(out, "year", st.year, first);
      kv_field_i(out, "month", st.month, first);
      kv_field_i(out, "day", st.day, first);
      kv_field_i(out, "hour", st.hour, first);
      kv_field_i(out, "minute", st.minute, first);
      kv_field_i(out, "second", st.second, first);
      out += '}';
      return true;
    }
#endif
    default:
      return false;
  }
}

// Parses a readable entity value back into a blob; false = not parseable.
static bool decode_entity_value(const char *s, size_t len, const RuntimeEntry &re, uint8_t *blob, size_t *blob_len) {
  switch (re.kind) {
    case EntityKind::BOOL: {
      bool v;
      if (len == 4 && memcmp(s, "true", 4) == 0) {
        v = true;
      } else if (len == 5 && memcmp(s, "false", 5) == 0) {
        v = false;
      } else if (len == 1 && (s[0] == '0' || s[0] == '1')) {
        v = s[0] == '1';
      } else {
        return false;
      }
      memcpy(blob, &v, sizeof(v));
      *blob_len = sizeof(bool);
      return true;
    }
    case EntityKind::FLOAT: {
      char b[32];
      if (len == 0 || len >= sizeof(b))
        return false;
      memcpy(b, s, len);
      b[len] = '\0';
      char *e = nullptr;
      float v = strtof(b, &e);
      if (e == nullptr || *e != '\0')
        return false;
      memcpy(blob, &v, sizeof(v));
      *blob_len = sizeof(float);
      return true;
    }
    case EntityKind::SELECT_INDEX: {
      char b[16];
      if (len == 0 || len >= sizeof(b))
        return false;
      memcpy(b, s, len);
      b[len] = '\0';
      char *e = nullptr;
      size_t v = static_cast<size_t>(strtoul(b, &e, 10));
      if (e == nullptr || *e != '\0')
        return false;
      memcpy(blob, &v, sizeof(v));
      *blob_len = sizeof(size_t);
      return true;
    }
    case EntityKind::STRING: {
      if (re.aux == 0 || len >= re.aux)
        return false;
      blob[0] = static_cast<uint8_t>(len);
      memcpy(blob + 1, s, len);
      memset(blob + 1 + len, 0, re.aux - 1 - len);
      *blob_len = re.aux;
      return true;
    }
    default:
      break;
  }
  // struct kinds: expect "{...}"
  if (len < 2 || s[0] != '{' || s[len - 1] != '}')
    return false;
  FieldReader r{s + 1, s + len - 1};
  long li;
  switch (re.kind) {
#ifdef USE_FAN
    case EntityKind::FAN: {
      fan::FanRestoreState st{};
      if (!r.b("state", st.state) || !r.i("speed", li))
        return false;
      st.speed = static_cast<int>(li);
      if (!r.b("oscillating", st.oscillating) || !r.i("direction", li))
        return false;
      st.direction = static_cast<fan::FanDirection>(li);
      if (!r.i("preset_mode", li))
        return false;
      st.preset_mode = static_cast<uint8_t>(li);
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
#ifdef USE_COVER
    case EntityKind::COVER: {
      cover::CoverRestoreState st{};
      // packed struct: fields cannot bind to float& — go through locals
      float pos, tilt;
      if (!r.f("position", pos) || !r.f("tilt", tilt))
        return false;
      st.position = pos;
      st.tilt = tilt;
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
#ifdef USE_VALVE
    case EntityKind::VALVE: {
      valve::ValveRestoreState st{};
      float pos;  // packed struct — see cover above
      if (!r.f("position", pos))
        return false;
      st.position = pos;
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
#ifdef USE_LIGHT
    case EntityKind::LIGHT: {
      light::LightStateRTCState st{};
      if (!r.b("state", st.state) || !r.f("brightness", st.brightness) ||
          !r.f("color_brightness", st.color_brightness) || !r.f("red", st.red) || !r.f("green", st.green) ||
          !r.f("blue", st.blue) || !r.f("white", st.white) || !r.f("color_temp", st.color_temp) ||
          !r.f("cold_white", st.cold_white) || !r.f("warm_white", st.warm_white) || !r.i("effect", li))
        return false;
      st.effect = static_cast<uint32_t>(li);
      if (!r.i("color_mode", li))
        return false;
      st.color_mode = static_cast<light::ColorMode>(li);
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
#ifdef USE_CLIMATE
    case EntityKind::CLIMATE: {
      climate::ClimateDeviceRestoreState st{};
      if (!r.i("mode", li))
        return false;
      st.mode = static_cast<climate::ClimateMode>(li);
      if (!r.b("uses_custom_fan_mode", st.uses_custom_fan_mode) || !r.i("fan_mode", li))
        return false;
      if (st.uses_custom_fan_mode) {
        st.custom_fan_mode = static_cast<uint8_t>(li);
      } else {
        st.fan_mode = static_cast<climate::ClimateFanMode>(li);
      }
      if (!r.b("uses_custom_preset", st.uses_custom_preset) || !r.i("preset", li))
        return false;
      if (st.uses_custom_preset) {
        st.custom_preset = static_cast<uint8_t>(li);
      } else {
        st.preset = static_cast<climate::ClimatePreset>(li);
      }
      if (!r.i("swing_mode", li))
        return false;
      st.swing_mode = static_cast<climate::ClimateSwingMode>(li);
      if (!r.f("target_temperature_low", st.target_temperature_low) ||
          !r.f("target_temperature_high", st.target_temperature_high) ||
          !r.f("target_humidity", st.target_humidity))
        return false;
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
    case EntityKind::MEDIA_VOLUME: {
      struct MediaVolumeState {
        float volume;
        bool is_muted;
      } st{};
      if (!r.f("volume", st.volume) || !r.b("is_muted", st.is_muted))
        return false;
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#ifdef USE_DATETIME_DATE
    case EntityKind::DATE: {
      datetime::DateEntityRestoreState st{};
      if (!r.i("year", li))
        return false;
      st.year = static_cast<uint16_t>(li);
      if (!r.i("month", li))
        return false;
      st.month = static_cast<uint8_t>(li);
      if (!r.i("day", li))
        return false;
      st.day = static_cast<uint8_t>(li);
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
#ifdef USE_DATETIME_TIME
    case EntityKind::TIME: {
      datetime::TimeEntityRestoreState st{};
      if (!r.i("hour", li))
        return false;
      st.hour = static_cast<uint8_t>(li);
      if (!r.i("minute", li))
        return false;
      st.minute = static_cast<uint8_t>(li);
      if (!r.i("second", li))
        return false;
      st.second = static_cast<uint8_t>(li);
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
#ifdef USE_DATETIME_DATETIME
    case EntityKind::DATETIME: {
      datetime::DateTimeEntityRestoreState st{};
      if (!r.i("year", li))
        return false;
      st.year = static_cast<uint16_t>(li);
      if (!r.i("month", li))
        return false;
      st.month = static_cast<uint8_t>(li);
      if (!r.i("day", li))
        return false;
      st.day = static_cast<uint8_t>(li);
      if (!r.i("hour", li))
        return false;
      st.hour = static_cast<uint8_t>(li);
      if (!r.i("minute", li))
        return false;
      st.minute = static_cast<uint8_t>(li);
      if (!r.i("second", li))
        return false;
      st.second = static_cast<uint8_t>(li);
      memcpy(blob, &st, sizeof(st));
      *blob_len = sizeof(st);
      return true;
    }
#endif
    default:
      return false;
  }
}

// ---- shared NVS plumbing ----

struct NvsEntry {
  uint32_t key;
  uint8_t blob[MAX_BLOB_LEN];
  size_t len;
};

static bool nvs_read_entry(nvs_handle_t handle, uint32_t key, NvsEntry &e) {
  char key_str[16];
  snprintf(key_str, sizeof(key_str), "%" PRIu32, key);
  e.key = key;
  e.len = 0;
  size_t len = 0;
  if (nvs_get_blob(handle, key_str, nullptr, &len) != ESP_OK || len == 0 || len > MAX_BLOB_LEN)
    return false;
  if (nvs_get_blob(handle, key_str, e.blob, &len) != ESP_OK)
    return false;
  e.len = len;
  return true;
}

template<typename EmitFn>
static size_t collect_entries(nvs_handle_t handle, const PrefSelection *sel, size_t count, bool restrict_to_selection,
                              const char *const *entity_names, size_t entity_name_count, EmitFn &&emit) {
  size_t n = 0;
  NvsEntry e;
  if (restrict_to_selection) {
    for (size_t i = 0; i < count; i++) {
      if (nvs_read_entry(handle, sel[i].key, e)) {
        emit(e, &sel[i]);
        n++;
      } else {
        ESP_LOGW(TAG, "Preference '%s' has no stored value yet — skipped", sel[i].name);
      }
    }
    // Selected ENTITY preferences: their keys live in the runtime registry.
    for (size_t i = 0; i < entity_name_count; i++) {
      const RuntimeEntry *re = runtime_by_name(entity_names[i], strlen(entity_names[i]), nullptr, 0);
      if (re == nullptr) {
        ESP_LOGW(TAG, "Entity preference '%s' is not registered — skipped", entity_names[i]);
        continue;
      }
      if (nvs_read_entry(handle, re->key, e)) {
        emit(e, nullptr);  // emit resolves the name via runtime_by_key
        n++;
      } else {
        ESP_LOGW(TAG, "Entity preference '%s' has no stored value yet — skipped", entity_names[i]);
      }
    }
    return n;
  }
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
  while (err == ESP_OK && it != nullptr) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    char *end = nullptr;
    unsigned long key = strtoul(info.key, &end, 10);
    if (end != nullptr && *end == '\0' && nvs_read_entry(handle, static_cast<uint32_t>(key), e)) {
      // Unrestricted mode still knows names/types for everything codegen
      // could see (all restore_value globals) — render those readable.
      emit(e, find_by_key(static_cast<uint32_t>(key), sel, count));
      n++;
    }
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  return n;
}

static PathStorage *resolve_file_target(const char *path, const char **rel) {
  if (global_storage_registry == nullptr) {
    ESP_LOGE(TAG, "Storage registry not available");
    return nullptr;
  }
  PathStorage *ps = global_storage_registry->resolve_path(path, rel);
  if (ps == nullptr || *rel == nullptr || (*rel)[0] == '\0' || strcmp(*rel, "/") == 0) {
    ESP_LOGE(TAG, "'%s' is not a file path on a mounted storage", path);
    return nullptr;
  }
  return ps;
}

// ---- export ----

bool preferences_export_to_storage(const char *path, const char *format, const PrefSelection *sel, size_t count,
                                   bool restrict_to_selection, const char *const *entity_names,
                                   size_t entity_name_count) {
  const bool as_json = strcmp(format, "json") == 0;
  if (!as_json && strcmp(format, "kv") != 0) {
    ESP_LOGE(TAG, "Unsupported format '%s'", format);
    return false;
  }
  const char *rel = nullptr;
  PathStorage *ps = resolve_file_target(path, &rel);
  if (ps == nullptr)
    return false;

  // Flush pending preference writes so NVS reflects the current state.
  global_preferences->sync();

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return false;
  }

  std::string out;
  size_t exported = 0;
  if (as_json) {
    auto buf = json::build_json([&](JsonObject root) {
      root["version"] = 1;
      JsonObject prefs = root["preferences"].to<JsonObject>();
      exported = collect_entries(handle, sel, count, restrict_to_selection, entity_names, entity_name_count,
                                [&](const NvsEntry &e, const PrefSelection *s) {
        char key_str[16];
        snprintf(key_str, sizeof(key_str), "%" PRIu32, e.key);
        std::string value;
        const RuntimeEntry *re = s == nullptr ? runtime_by_key(e.key, restrict_to_selection ? entity_names : nullptr,
                                                               entity_name_count)
                                              : nullptr;
        if (s != nullptr) {
          encode_value(value, *s, e.blob, e.len);
        } else if (re == nullptr || !encode_entity_value(value, *re, e.blob, e.len)) {
          value = HEX_PREFIX;
          append_hex(value, e.blob, e.len);
        }
        prefs[s != nullptr ? s->name : (re != nullptr ? re->name : key_str)] = value;
      });
    });
    out.assign(buf.data(), buf.size());
  } else {
    out += "# ESPHome preferences export (kv v1)\n";
    out += "# <global id or numeric NVS key>=<typed value or hex:...>\n";
    exported = collect_entries(handle, sel, count, restrict_to_selection, entity_names, entity_name_count,
                                [&](const NvsEntry &e, const PrefSelection *s) {
      if (s != nullptr) {
        out += s->name;
        out += '=';
        encode_value(out, *s, e.blob, e.len);
      } else {
        const RuntimeEntry *re =
            runtime_by_key(e.key, restrict_to_selection ? entity_names : nullptr, entity_name_count);
        if (re != nullptr) {
          out += re->name;
        } else {
          char key_str[16];
          snprintf(key_str, sizeof(key_str), "%" PRIu32, e.key);
          out += key_str;
        }
        out += '=';
        if (re == nullptr || !encode_entity_value(out, *re, e.blob, e.len)) {
          out += HEX_PREFIX;
          append_hex(out, e.blob, e.len);
        }
      }
      out += '\n';
    });
  }
  nvs_close(handle);

  StorageError werr = write_file(ps, rel, reinterpret_cast<const uint8_t *>(out.data()), out.size());
  if (werr != StorageError::OK) {
    ESP_LOGE(TAG, "Writing export failed (%s)", error_to_string(werr));
    return false;
  }
  ESP_LOGI(TAG, "Exported %zu preference(s), %zu bytes to '%s'", exported, out.size(), path);
  return true;
}

// ---- import ----

// Writes one parsed name/value pair to NVS; shared by both formats.
static bool import_one(nvs_handle_t handle, const char *name, size_t name_len, const char *value, size_t value_len,
                       const PrefSelection *sel, size_t count, bool restrict_to_selection,
                       const char *const *entity_names, size_t entity_name_count, size_t &imported,
                       size_t &skipped) {
  const PrefSelection *s = find_by_name(name, name_len, sel, count);
  uint32_t key;
  if (s != nullptr) {
    key = s->key;
  } else if (const RuntimeEntry *re = runtime_by_name(
                 name, name_len, restrict_to_selection ? entity_names : nullptr, entity_name_count)) {
    key = re->key;
    // typed parse first; hex: prefix (and stale-format hex) still accepted below
    if (value_len < strlen(HEX_PREFIX) || memcmp(value, HEX_PREFIX, strlen(HEX_PREFIX)) != 0) {
      uint8_t blob[MAX_BLOB_LEN];
      size_t blob_len = 0;
      if (decode_entity_value(value, value_len, *re, blob, &blob_len)) {
        char key_str[16];
        snprintf(key_str, sizeof(key_str), "%" PRIu32, key);
        esp_err_t err = nvs_set_blob(handle, key_str, blob, blob_len);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "nvs_set_blob('%s') failed: %s", key_str, esp_err_to_name(err));
          return false;
        }
        imported++;
        return true;
      }
    }
  } else {
    char buf[16];
    if (name_len == 0 || name_len >= sizeof(buf)) {
      skipped++;
      return true;
    }
    memcpy(buf, name, name_len);
    buf[name_len] = '\0';
    char *end = nullptr;
    unsigned long v = strtoul(buf, &end, 10);
    if (end == nullptr || *end != '\0' ||
        (restrict_to_selection && find_by_key(v, sel, count) == nullptr)) {
      skipped++;  // unknown name, or filtered out by the configured selection
      return true;
    }
    key = static_cast<uint32_t>(v);
    s = find_by_key(key, sel, count);
  }

  uint8_t blob[MAX_BLOB_LEN];
  size_t blob_len = 0;
  bool ok;
  if (s != nullptr) {
    ok = decode_value(value, value_len, *s, blob, &blob_len);
  } else {
    // untyped numeric-key entry: hex only (with or without prefix)
    const char *hex = value;
    size_t hex_len = value_len;
    if (hex_len >= strlen(HEX_PREFIX) && memcmp(hex, HEX_PREFIX, strlen(HEX_PREFIX)) == 0) {
      hex += strlen(HEX_PREFIX);
      hex_len -= strlen(HEX_PREFIX);
    }
    ok = parse_hex(hex, hex_len, blob, &blob_len);
  }
  if (!ok) {
    ESP_LOGW(TAG, "Skipping malformed value for '%.*s'", (int) name_len, name);
    skipped++;
    return true;
  }

  char key_str[16];
  snprintf(key_str, sizeof(key_str), "%" PRIu32, key);
  esp_err_t err = nvs_set_blob(handle, key_str, blob, blob_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_set_blob('%s') failed: %s", key_str, esp_err_to_name(err));
    return false;
  }
  imported++;
  return true;
}

bool preferences_import_from_storage(const char *path, const char *format, bool reboot, const PrefSelection *sel,
                                     size_t count, bool restrict_to_selection, const char *const *entity_names,
                                     size_t entity_name_count) {
  const bool as_json = strcmp(format, "json") == 0;
  if (!as_json && strcmp(format, "kv") != 0) {
    ESP_LOGE(TAG, "Unsupported format '%s'", format);
    return false;
  }
  const char *rel = nullptr;
  PathStorage *ps = resolve_file_target(path, &rel);
  if (ps == nullptr)
    return false;

  RamBuffer buf;
  size_t size = 0;
  StorageError rerr = read_file(ps, rel, buf, &size);
  if (rerr != StorageError::OK) {
    ESP_LOGE(TAG, "Reading '%s' failed (%s)", path, error_to_string(rerr));
    return false;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return false;
  }

  size_t imported = 0, skipped = 0;
  bool ok = true;
  if (as_json) {
    ok = json::parse_json(buf.get(), size, [&](JsonObject root) -> bool {
      JsonObject prefs = root["preferences"];
      if (prefs.isNull()) {
        ESP_LOGE(TAG, "JSON import: missing 'preferences' object");
        return false;
      }
      for (JsonPair kv : prefs) {
        const char *value = kv.value().as<const char *>();
        if (value == nullptr) {
          ESP_LOGW(TAG, "Skipping non-string value for '%s'", kv.key().c_str());
          skipped++;
          continue;
        }
        if (!import_one(handle, kv.key().c_str(), strlen(kv.key().c_str()), value, strlen(value), sel, count,
                        restrict_to_selection, entity_names, entity_name_count, imported, skipped))
          return false;
      }
      return true;
    });
  } else {
    const char *data = reinterpret_cast<const char *>(buf.get());
    size_t pos = 0;
    while (ok && pos < size) {
      size_t eol = pos;
      while (eol < size && data[eol] != '\n')
        eol++;
      size_t line_len = eol - pos;
      while (line_len > 0 && (data[pos + line_len - 1] == '\r' || data[pos + line_len - 1] == ' '))
        line_len--;
      const char *line = data + pos;
      pos = eol + 1;
      if (line_len == 0 || line[0] == '#')
        continue;
      const char *eq = static_cast<const char *>(memchr(line, '=', line_len));
      if (eq == nullptr) {
        ESP_LOGW(TAG, "Skipping malformed line (no '=')");
        skipped++;
        continue;
      }
      ok = import_one(handle, line, eq - line, eq + 1, line_len - (eq + 1 - line), sel, count,
                      restrict_to_selection, entity_names, entity_name_count, imported, skipped);
    }
  }
  if (ok && (err = nvs_commit(handle)) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    ok = false;
  }
  nvs_close(handle);

  if (!ok)
    return false;
  ESP_LOGI(TAG, "Imported %zu preference(s), skipped %zu — values take effect after reboot", imported, skipped);
  if (reboot) {
    ESP_LOGI(TAG, "Rebooting to apply imported preferences");
    App.safe_reboot();
  }
  return true;
}

}  // namespace esphome::storage

#endif  // USE_STORAGE_PREFERENCES && USE_ESP32
