#include "nvs_guard.h"

#if defined(USE_ESP32) && defined(USE_ESP32_NVS_GUARD)

#include "esphome/core/build_info_data.h"
#include "esphome/core/helpers.h"
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace esphome::esp32::nvs_guard {

static constexpr const char *PARTITION = NVS_DEFAULT_PART_NAME;
static constexpr const char *META_NS = "nvs_meta";
static constexpr const char *PREF_NS = "esphome";
static constexpr uint8_t META_LAYOUT = 1;
static constexpr uint32_t SNAP_MAGIC = 0x4253564E;
static constexpr uint8_t SNAP_VERSION = 1;
static constexpr size_t SNAP_HEADER = 16;
static constexpr size_t MAX_CLAIMS = 256;
static constexpr size_t VERSION_MAX = 32;
static constexpr size_t PROJECT_MAX = 64;

static const char *const PROTECTED[] = {ESP32_NVS_GUARD_PROTECTED};

struct Claim {
  uint32_t key;
  uint32_t len;
};

static State s_state;
static Claim *s_claims = nullptr;
static size_t s_claim_count = 0;

static void *alloc(size_t size) {
  void *mem = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return mem != nullptr ? mem : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

const State &state() { return s_state; }

const char *result_name(Result result) {
  switch (result) {
    case Result::FRESH:
      return "fresh";
    case Result::SAME:
      return "same";
    case Result::ADOPTED:
      return "adopted";
    case Result::UPGRADED:
      return "upgraded";
    case Result::DOWNGRADED:
      return "downgraded";
    case Result::FOREIGN:
      return "foreign";
    case Result::CORRUPT:
      return "corrupt";
  }
  return "?";
}

const char *policy_name(Policy policy) {
  switch (policy) {
    case Policy::KEEP:
      return "keep";
    case Policy::INVALIDATE_CHANGED:
      return "invalidate_changed";
    case Policy::RESET:
      return "reset";
    case Policy::READ_ONLY:
      return "read_only";
    case Policy::RESET_ALL:
      return "reset_all";
  }
  return "?";
}

bool parse_policy(const char *text, Policy *out) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(Policy::RESET_ALL); i++) {
    if (strcmp(text, policy_name(static_cast<Policy>(i))) == 0) {
      *out = static_cast<Policy>(i);
      return true;
    }
  }
  return false;
}

bool is_protected(const char *ns) {
  for (const char *name : PROTECTED) {
    if (strcmp(name, ns) == 0)
      return true;
  }
  return false;
}

static size_t type_size(nvs_type_t type) {
  switch (type) {
    case NVS_TYPE_U8:
    case NVS_TYPE_I8:
      return 1;
    case NVS_TYPE_U16:
    case NVS_TYPE_I16:
      return 2;
    case NVS_TYPE_U32:
    case NVS_TYPE_I32:
      return 4;
    case NVS_TYPE_U64:
    case NVS_TYPE_I64:
      return 8;
    default:
      return 0;
  }
}

static bool value_size(nvs_handle_t handle, const nvs_entry_info_t &info, size_t *size) {
  if (info.type == NVS_TYPE_STR)
    return nvs_get_str(handle, info.key, nullptr, size) == ESP_OK;
  if (info.type == NVS_TYPE_BLOB)
    return nvs_get_blob(handle, info.key, nullptr, size) == ESP_OK;
  *size = type_size(info.type);
  return *size != 0;
}

static bool value_read(nvs_handle_t handle, const nvs_entry_info_t &info, uint8_t *out, size_t size) {
  switch (info.type) {
    case NVS_TYPE_U8:
      return nvs_get_u8(handle, info.key, out) == ESP_OK;
    case NVS_TYPE_I8:
      return nvs_get_i8(handle, info.key, reinterpret_cast<int8_t *>(out)) == ESP_OK;
    case NVS_TYPE_U16: {
      uint16_t v;
      bool ok = nvs_get_u16(handle, info.key, &v) == ESP_OK;
      memcpy(out, &v, sizeof(v));
      return ok;
    }
    case NVS_TYPE_I16: {
      int16_t v;
      bool ok = nvs_get_i16(handle, info.key, &v) == ESP_OK;
      memcpy(out, &v, sizeof(v));
      return ok;
    }
    case NVS_TYPE_U32: {
      uint32_t v;
      bool ok = nvs_get_u32(handle, info.key, &v) == ESP_OK;
      memcpy(out, &v, sizeof(v));
      return ok;
    }
    case NVS_TYPE_I32: {
      int32_t v;
      bool ok = nvs_get_i32(handle, info.key, &v) == ESP_OK;
      memcpy(out, &v, sizeof(v));
      return ok;
    }
    case NVS_TYPE_U64: {
      uint64_t v;
      bool ok = nvs_get_u64(handle, info.key, &v) == ESP_OK;
      memcpy(out, &v, sizeof(v));
      return ok;
    }
    case NVS_TYPE_I64: {
      int64_t v;
      bool ok = nvs_get_i64(handle, info.key, &v) == ESP_OK;
      memcpy(out, &v, sizeof(v));
      return ok;
    }
    case NVS_TYPE_STR:
      return nvs_get_str(handle, info.key, reinterpret_cast<char *>(out), &size) == ESP_OK;
    case NVS_TYPE_BLOB:
      return nvs_get_blob(handle, info.key, out, &size) == ESP_OK;
    default:
      return false;
  }
}

static esp_err_t value_write(nvs_handle_t handle, const char *key, uint8_t type, const uint8_t *data, size_t size) {
  switch (static_cast<nvs_type_t>(type)) {
    case NVS_TYPE_U8:
      return nvs_set_u8(handle, key, data[0]);
    case NVS_TYPE_I8:
      return nvs_set_i8(handle, key, static_cast<int8_t>(data[0]));
    case NVS_TYPE_U16: {
      uint16_t v;
      memcpy(&v, data, sizeof(v));
      return nvs_set_u16(handle, key, v);
    }
    case NVS_TYPE_I16: {
      int16_t v;
      memcpy(&v, data, sizeof(v));
      return nvs_set_i16(handle, key, v);
    }
    case NVS_TYPE_U32: {
      uint32_t v;
      memcpy(&v, data, sizeof(v));
      return nvs_set_u32(handle, key, v);
    }
    case NVS_TYPE_I32: {
      int32_t v;
      memcpy(&v, data, sizeof(v));
      return nvs_set_i32(handle, key, v);
    }
    case NVS_TYPE_U64: {
      uint64_t v;
      memcpy(&v, data, sizeof(v));
      return nvs_set_u64(handle, key, v);
    }
    case NVS_TYPE_I64: {
      int64_t v;
      memcpy(&v, data, sizeof(v));
      return nvs_set_i64(handle, key, v);
    }
    case NVS_TYPE_STR:
      if (size == 0 || data[size - 1] != '\0')
        return ESP_ERR_INVALID_ARG;
      return nvs_set_str(handle, key, reinterpret_cast<const char *>(data));
    case NVS_TYPE_BLOB:
      return nvs_set_blob(handle, key, data, size);
    default:
      return ESP_ERR_INVALID_ARG;
  }
}

template<typename F> static void for_each_entry(bool protected_only, F &&visit) {
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(PARTITION, nullptr, NVS_TYPE_ANY, &it);
  while (err == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    if (strcmp(info.namespace_name, META_NS) != 0 && (!protected_only || is_protected(info.namespace_name)))
      visit(info);
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
}

size_t snapshot(bool protected_only, uint8_t **out) {
  *out = nullptr;
  size_t payload = 0;
  uint16_t count = 0;
  for_each_entry(protected_only, [&](const nvs_entry_info_t &info) {
    nvs_handle_t handle;
    if (nvs_open(info.namespace_name, NVS_READONLY, &handle) != ESP_OK)
      return;
    size_t size = 0;
    if (value_size(handle, info, &size) && size <= UINT16_MAX && count < UINT16_MAX) {
      payload += 2 + strlen(info.namespace_name) + strlen(info.key) + 3 + size;
      count++;
    }
    nvs_close(handle);
  });
  const size_t total = SNAP_HEADER + payload;
  auto *buf = static_cast<uint8_t *>(alloc(total));
  if (buf == nullptr)
    return 0;
  size_t at = SNAP_HEADER;
  uint16_t written = 0;
  for_each_entry(protected_only, [&](const nvs_entry_info_t &info) {
    nvs_handle_t handle;
    if (written == count || nvs_open(info.namespace_name, NVS_READONLY, &handle) != ESP_OK)
      return;
    size_t size = 0;
    const size_t ns_len = strlen(info.namespace_name);
    const size_t key_len = strlen(info.key);
    if (value_size(handle, info, &size) && size <= UINT16_MAX && at + 2 + ns_len + key_len + 3 + size <= total) {
      uint8_t *p = buf + at;
      *p++ = static_cast<uint8_t>(ns_len);
      memcpy(p, info.namespace_name, ns_len);
      p += ns_len;
      *p++ = static_cast<uint8_t>(key_len);
      memcpy(p, info.key, key_len);
      p += key_len;
      *p++ = static_cast<uint8_t>(info.type);
      *p++ = static_cast<uint8_t>(size & 0xFF);
      *p++ = static_cast<uint8_t>(size >> 8);
      if (value_read(handle, info, p, size)) {
        at = static_cast<size_t>(p - buf) + size;
        written++;
      }
    }
    nvs_close(handle);
  });
  const uint32_t size = static_cast<uint32_t>(at - SNAP_HEADER);
  const uint32_t crc = esp_rom_crc32_le(0, buf + SNAP_HEADER, size);
  memcpy(buf, &SNAP_MAGIC, 4);
  buf[4] = SNAP_VERSION;
  buf[5] = 0;
  memcpy(buf + 6, &written, 2);
  memcpy(buf + 8, &size, 4);
  memcpy(buf + 12, &crc, 4);
  *out = buf;
  return at;
}

bool restore(const uint8_t *data, size_t size, bool protected_only, uint16_t *count) {
  *count = 0;
  uint32_t magic = 0, payload = 0, crc = 0;
  uint16_t entries = 0;
  if (data == nullptr || size < SNAP_HEADER)
    return false;
  memcpy(&magic, data, 4);
  memcpy(&entries, data + 6, 2);
  memcpy(&payload, data + 8, 4);
  memcpy(&crc, data + 12, 4);
  if (magic != SNAP_MAGIC || data[4] != SNAP_VERSION || SNAP_HEADER + payload > size ||
      esp_rom_crc32_le(0, data + SNAP_HEADER, payload) != crc)
    return false;
  const uint8_t *p = data + SNAP_HEADER;
  const uint8_t *end = p + payload;
  bool ok = true;
  for (uint16_t i = 0; i < entries; i++) {
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    if (p >= end || *p >= sizeof(ns) || p + 1 + *p >= end)
      return false;
    const uint8_t ns_len = *p++;
    memcpy(ns, p, ns_len);
    ns[ns_len] = '\0';
    p += ns_len;
    if (*p >= sizeof(key) || p + 1 + *p + 3 > end)
      return false;
    const uint8_t key_len = *p++;
    memcpy(key, p, key_len);
    key[key_len] = '\0';
    p += key_len;
    const uint8_t type = *p++;
    const size_t len = static_cast<size_t>(p[0]) | (static_cast<size_t>(p[1]) << 8);
    p += 2;
    if (p + len > end)
      return false;
    if (!protected_only || is_protected(ns)) {
      nvs_handle_t handle;
      if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        if (value_write(handle, key, type, p, len) == ESP_OK) {
          (*count)++;
        } else {
          ok = false;
        }
        nvs_commit(handle);
        nvs_close(handle);
      } else {
        ok = false;
      }
    }
    p += len;
  }
  return ok;
}

void release_backup() {
  heap_caps_free(s_state.backup);
  s_state.backup = nullptr;
  s_state.backup_size = 0;
}

static bool erase_and_init() {
  nvs_flash_erase();
  return nvs_flash_init() == ESP_OK;
}

bool reset(bool keep_protected, uint16_t *restored) {
  *restored = 0;
  release_backup();
  s_state.backup_size = snapshot(false, &s_state.backup);
  if (!erase_and_init())
    return false;
  if (keep_protected && s_state.backup != nullptr)
    restore(s_state.backup, s_state.backup_size, true, restored);
  return true;
}

size_t verify(uint16_t *unreadable) {
  *unreadable = 0;
  size_t total = 0;
  uint8_t scratch[64];
  for_each_entry(false, [&](const nvs_entry_info_t &info) {
    total++;
    nvs_handle_t handle;
    if (nvs_open(info.namespace_name, NVS_READONLY, &handle) != ESP_OK) {
      (*unreadable)++;
      return;
    }
    size_t size = 0;
    bool ok = value_size(handle, info, &size);
    if (ok) {
      uint8_t *buf = size <= sizeof(scratch)
                         ? scratch
                         : static_cast<uint8_t *>(alloc(size));
      ok = buf != nullptr && value_read(handle, info, buf, size);
      if (buf != scratch)
        heap_caps_free(buf);
    }
    if (!ok)
      (*unreadable)++;
    nvs_close(handle);
  });
  return total;
}

static int compare_versions(const char *a, const char *b) {
  if (a[0] == '\0' || b[0] == '\0')
    return 0;
  while (*a != '\0' || *b != '\0') {
    char *next_a = nullptr;
    char *next_b = nullptr;
    const unsigned long va = strtoul(a, &next_a, 10);
    const unsigned long vb = strtoul(b, &next_b, 10);
    if (next_a == a || next_b == b)
      return 0;
    if (va != vb)
      return va < vb ? -1 : 1;
    a = *next_a == '.' ? next_a + 1 : next_a;
    b = *next_b == '.' ? next_b + 1 : next_b;
    if ((*a != '\0' && !isdigit(static_cast<unsigned char>(*a))) ||
        (*b != '\0' && !isdigit(static_cast<unsigned char>(*b))))
      return 0;
  }
  return 0;
}

static const char *own_project() {
#ifdef ESPHOME_PROJECT_NAME
  return ESPHOME_PROJECT_NAME;
#else
  return "";
#endif
}

static const char *own_version() {
#ifdef ESPHOME_PROJECT_VERSION
  return ESPHOME_PROJECT_VERSION;
#else
  return "";
#endif
}

static Policy policy_for(Result result) {
  switch (result) {
    case Result::UPGRADED:
      return static_cast<Policy>(ESP32_NVS_GUARD_ON_UPGRADE);
    case Result::DOWNGRADED:
      return static_cast<Policy>(ESP32_NVS_GUARD_ON_DOWNGRADE);
    case Result::FOREIGN:
      return static_cast<Policy>(ESP32_NVS_GUARD_ON_FOREIGN);
    case Result::CORRUPT:
      return static_cast<Policy>(ESP32_NVS_GUARD_ON_CORRUPT);
    default:
      return Policy::KEEP;
  }
}

static void write_meta() {
  nvs_handle_t meta;
  if (nvs_open(META_NS, NVS_READWRITE, &meta) != ESP_OK)
    return;
  nvs_set_u8(meta, "layout", META_LAYOUT);
  nvs_set_u32(meta, "hash", ESPHOME_CONFIG_HASH);
  nvs_set_i64(meta, "time", static_cast<int64_t>(ESPHOME_BUILD_TIME));
  nvs_set_str(meta, "project", own_project());
  nvs_set_str(meta, "version", own_version());
  nvs_set_u8(meta, "result", static_cast<uint8_t>(s_state.result));
  nvs_commit(meta);
  nvs_close(meta);
}

static Result classify(nvs_handle_t meta) {
  uint32_t hash = 0;
  int64_t time = 0;
  if (nvs_get_u32(meta, "hash", &hash) != ESP_OK || nvs_get_i64(meta, "time", &time) != ESP_OK) {
    nvs_iterator_t it = nullptr;
    const bool data = nvs_entry_find(PARTITION, PREF_NS, NVS_TYPE_ANY, &it) == ESP_OK;
    nvs_release_iterator(it);
    return data ? Result::ADOPTED : Result::FRESH;
  }
  char project[PROJECT_MAX] = "";
  char version[VERSION_MAX] = "";
  size_t len = sizeof(project);
  if (nvs_get_str(meta, "project", project, &len) != ESP_OK)
    project[0] = '\0';
  len = sizeof(version);
  if (nvs_get_str(meta, "version", version, &len) != ESP_OK)
    version[0] = '\0';
  if (project[0] != '\0' && own_project()[0] != '\0' && strcmp(project, own_project()) != 0)
    return Result::FOREIGN;
  if (hash == ESPHOME_CONFIG_HASH && time == static_cast<int64_t>(ESPHOME_BUILD_TIME))
    return Result::SAME;
  const int cmp = compare_versions(own_version(), version);
  if (cmp != 0)
    return cmp > 0 ? Result::UPGRADED : Result::DOWNGRADED;
  return static_cast<int64_t>(ESPHOME_BUILD_TIME) < time ? Result::DOWNGRADED : Result::UPGRADED;
}

static void apply(Policy policy) {
  s_state.applied = policy;
  switch (policy) {
    case Policy::INVALIDATE_CHANGED:
      s_state.sweep_pending = true;
      break;
    case Policy::READ_ONLY:
      s_state.read_only = true;
      break;
    case Policy::RESET:
    case Policy::RESET_ALL:
      reset(policy == Policy::RESET, &s_state.restored);
      break;
    case Policy::KEEP:
      break;
  }
}

bool open_partition(bool *nvs_ready) {
  esp_err_t err = nvs_flash_init();
  s_state.init_err = err;
  nvs_handle_t meta = 0;
  if (err == ESP_OK)
    err = nvs_open(META_NS, NVS_READWRITE, &meta);
  if (err != ESP_OK) {
    s_state.result = Result::CORRUPT;
    s_state.applied = policy_for(Result::CORRUPT);
    if (s_state.applied == Policy::RESET || s_state.applied == Policy::RESET_ALL) {
      s_state.applied = Policy::RESET_ALL;
      *nvs_ready = erase_and_init();
      if (*nvs_ready)
        write_meta();
    } else {
      s_state.applied = Policy::KEEP;
      *nvs_ready = s_state.init_err == ESP_OK;
      s_state.read_only = true;
    }
    return *nvs_ready;
  }
  uint8_t request = 0;
  if (nvs_get_u8(meta, "request", &request) == ESP_OK) {
    nvs_erase_key(meta, "request");
    nvs_commit(meta);
    s_state.requested = request <= static_cast<uint8_t>(Policy::RESET_ALL);
  }
  s_state.result = classify(meta);
  nvs_close(meta);
  apply(s_state.requested ? static_cast<Policy>(request) : policy_for(s_state.result));
  if (!s_state.read_only)
    write_meta();
  *nvs_ready = true;
  return true;
}

void claim(uint32_t key, size_t len) {
  if (!s_state.sweep_pending)
    return;
  if (s_claims == nullptr) {
    s_claims = static_cast<Claim *>(alloc(MAX_CLAIMS * sizeof(Claim)));
    if (s_claims == nullptr)
      return;
  }
  for (size_t i = 0; i < s_claim_count; i++) {
    if (s_claims[i].key == key)
      return;
  }
  if (s_claim_count < MAX_CLAIMS)
    s_claims[s_claim_count++] = Claim{key, static_cast<uint32_t>(len)};
}

uint16_t sweep(uint32_t handle) {
  uint16_t erased = 0;
  if (s_state.sweep_pending && handle != 0 && s_claims != nullptr) {
    for (size_t i = 0; i < s_claim_count; i++) {
      char key[UINT32_MAX_STR_SIZE];
      uint32_to_str(key, s_claims[i].key);
      size_t stored = 0;
      if (nvs_get_blob(handle, key, nullptr, &stored) == ESP_OK && stored != s_claims[i].len &&
          nvs_erase_key(handle, key) == ESP_OK)
        erased++;
    }
    if (erased != 0)
      nvs_commit(handle);
  }
  heap_caps_free(s_claims);
  s_claims = nullptr;
  s_claim_count = 0;
  s_state.sweep_pending = false;
  s_state.invalidated = erased;
  return erased;
}

bool request(Policy policy) {
  nvs_handle_t meta;
  if (nvs_open(META_NS, NVS_READWRITE, &meta) != ESP_OK)
    return false;
  const bool ok = nvs_set_u8(meta, "request", static_cast<uint8_t>(policy)) == ESP_OK && nvs_commit(meta) == ESP_OK;
  nvs_close(meta);
  return ok;
}

}

#endif
