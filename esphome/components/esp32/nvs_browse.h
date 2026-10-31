#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <nvs.h>
#include <nvs_flash.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace esphome::esp32::nvs_browse {

static constexpr const char *PARTITION = NVS_DEFAULT_PART_NAME;
static constexpr size_t MAX_NAMESPACES = 16;

struct Namespace {
  char name[NVS_NS_NAME_MAX_SIZE];
  uint16_t count;
};

inline const char *type_name(nvs_type_t type) {
  switch (type) {
    case NVS_TYPE_U8:
      return "u8";
    case NVS_TYPE_I8:
      return "i8";
    case NVS_TYPE_U16:
      return "u16";
    case NVS_TYPE_I16:
      return "i16";
    case NVS_TYPE_U32:
      return "u32";
    case NVS_TYPE_I32:
      return "i32";
    case NVS_TYPE_U64:
      return "u64";
    case NVS_TYPE_I64:
      return "i64";
    case NVS_TYPE_STR:
      return "str";
    case NVS_TYPE_BLOB:
      return "blob";
    default:
      return "?";
  }
}

inline size_t namespaces(Namespace *out, size_t max) {
  size_t found = 0;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(PARTITION, nullptr, NVS_TYPE_ANY, &it);
  while (err == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    size_t i = 0;
    while (i < found && strcmp(out[i].name, info.namespace_name) != 0)
      i++;
    if (i == found && found < max) {
      strncpy(out[found].name, info.namespace_name, sizeof(out[found].name) - 1);
      out[found].name[sizeof(out[found].name) - 1] = '\0';
      out[found].count = 0;
      found++;
    }
    if (i < found)
      out[i].count++;
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  return found;
}

inline void value_text(nvs_handle_t handle, const nvs_entry_info_t &info, char *out, size_t len) {
  out[0] = '\0';
  switch (info.type) {
    case NVS_TYPE_U8: {
      uint8_t v = 0;
      if (nvs_get_u8(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%u", static_cast<unsigned>(v));
      break;
    }
    case NVS_TYPE_I8: {
      int8_t v = 0;
      if (nvs_get_i8(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%d", static_cast<int>(v));
      break;
    }
    case NVS_TYPE_U16: {
      uint16_t v = 0;
      if (nvs_get_u16(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%u", static_cast<unsigned>(v));
      break;
    }
    case NVS_TYPE_I16: {
      int16_t v = 0;
      if (nvs_get_i16(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%d", static_cast<int>(v));
      break;
    }
    case NVS_TYPE_U32: {
      uint32_t v = 0;
      if (nvs_get_u32(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%" PRIu32, v);
      break;
    }
    case NVS_TYPE_I32: {
      int32_t v = 0;
      if (nvs_get_i32(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%" PRId32, v);
      break;
    }
    case NVS_TYPE_U64: {
      uint64_t v = 0;
      if (nvs_get_u64(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%" PRIu64, v);
      break;
    }
    case NVS_TYPE_I64: {
      int64_t v = 0;
      if (nvs_get_i64(handle, info.key, &v) == ESP_OK)
        snprintf(out, len, "%" PRId64, v);
      break;
    }
    case NVS_TYPE_STR: {
      size_t size = 0;
      if (nvs_get_str(handle, info.key, nullptr, &size) != ESP_OK)
        break;
      if (size <= len) {
        nvs_get_str(handle, info.key, out, &size);
        for (char *c = out; *c != '\0'; c++) {
          if (*c == '"' || *c == '\\' || static_cast<unsigned char>(*c) < 0x20)
            *c = ' ';
        }
      } else {
        snprintf(out, len, "%u B", static_cast<unsigned>(size));
      }
      break;
    }
    case NVS_TYPE_BLOB: {
      size_t size = 0;
      if (nvs_get_blob(handle, info.key, nullptr, &size) == ESP_OK)
        snprintf(out, len, "%u B", static_cast<unsigned>(size));
      break;
    }
    default:
      break;
  }
}

template<typename F> size_t entries(const char *name, size_t skip, size_t max, F &&row) {
  nvs_handle_t handle = 0;
  const bool opened = nvs_open_from_partition(PARTITION, name, NVS_READONLY, &handle) == ESP_OK;
  size_t total = 0;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(PARTITION, name, NVS_TYPE_ANY, &it);
  char value[48];
  while (err == ESP_OK) {
    if (total >= skip && total < skip + max) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      if (opened)
        value_text(handle, info, value, sizeof(value));
      else
        value[0] = '\0';
      row(static_cast<const char *>(info.key), type_name(info.type), static_cast<const char *>(value));
    }
    total++;
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  if (opened)
    nvs_close(handle);
  return total;
}

inline bool erase(const char *name, const char *key) {
  nvs_handle_t handle = 0;
  if (nvs_open_from_partition(PARTITION, name, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  esp_err_t err = key != nullptr && key[0] != '\0' ? nvs_erase_key(handle, key) : nvs_erase_all(handle);
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}

inline bool erase_everything() { return nvs_flash_erase() == ESP_OK; }

inline void usage_text(char *out, size_t len) {
  nvs_stats_t stats{};
  if (nvs_get_stats(PARTITION, &stats) == ESP_OK) {
    snprintf(out, len, "%u of %u entries used", static_cast<unsigned>(stats.used_entries),
             static_cast<unsigned>(stats.total_entries));
  } else {
    out[0] = '\0';
  }
}

}

#endif
