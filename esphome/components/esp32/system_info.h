#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs.h>
#include <sdkconfig.h>
#ifdef CONFIG_SPIRAM
#include <esp_psram.h>
#endif
#ifdef CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include <esp_core_dump.h>
#endif

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace esphome::esp32::system_info {

inline void size_text(char *out, size_t len, uint64_t bytes) {
  if (bytes >= 1024 * 1024) {
    snprintf(out, len, "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(out, len, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(out, len, "%u B", static_cast<unsigned>(bytes));
  }
}

inline void chip_text(char *out, size_t len) {
  esp_chip_info_t info;
  esp_chip_info(&info);
#ifdef CONFIG_ESP32P4_SELECTS_REV_LESS_V3
  const char *sample = " (engineering sample)";
#else
  const char *sample = "";
#endif
  snprintf(out, len, "%s v%u.%u%s", ESPHOME_VARIANT, info.revision / 100, info.revision % 100, sample);
}

inline void cpu_text(char *out, size_t len) {
  esp_chip_info_t info;
  esp_chip_info(&info);
  snprintf(out, len, "%u MHz, %u cores", static_cast<unsigned>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
           static_cast<unsigned>(info.cores));
}

inline uint32_t flash_size() {
  uint32_t size = 0;
  esp_flash_get_size(nullptr, &size);
  return size;
}

inline void flash_text(char *out, size_t len) {
#if defined(CONFIG_ESPTOOLPY_FLASHMODE_QIO)
  const char *mode = "QIO";
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_QOUT)
  const char *mode = "QOUT";
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_DIO)
  const char *mode = "DIO";
#else
  const char *mode = "DOUT";
#endif
  char size[16];
  size_text(size, sizeof(size), flash_size());
  snprintf(out, len, "%s, %s %u MHz", size, mode, static_cast<unsigned>(CONFIG_ESPTOOLPY_FLASHFREQ_VAL));
}

inline void psram_text(char *out, size_t len) {
#ifdef CONFIG_SPIRAM
  char size[16];
  size_text(size, sizeof(size), esp_psram_get_size());
  snprintf(out, len, "%s, %u MHz", size, static_cast<unsigned>(CONFIG_SPIRAM_SPEED));
#else
  snprintf(out, len, "none");
#endif
}

inline const char *type_name(const esp_partition_t *p) {
  return p->type == ESP_PARTITION_TYPE_APP ? "app" : "data";
}

inline void subtype_name(const esp_partition_t *p, char *out, size_t len) {
  if (p->type == ESP_PARTITION_TYPE_APP) {
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
      snprintf(out, len, "factory");
    } else if (p->subtype == ESP_PARTITION_SUBTYPE_APP_TEST) {
      snprintf(out, len, "test");
    } else {
      snprintf(out, len, "ota_%u", static_cast<unsigned>(p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN));
    }
    return;
  }
  switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA:
      snprintf(out, len, "ota");
      break;
    case ESP_PARTITION_SUBTYPE_DATA_PHY:
      snprintf(out, len, "phy");
      break;
    case ESP_PARTITION_SUBTYPE_DATA_NVS:
      snprintf(out, len, "nvs");
      break;
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
      snprintf(out, len, "coredump");
      break;
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
      snprintf(out, len, "nvs_keys");
      break;
    case ESP_PARTITION_SUBTYPE_DATA_FAT:
      snprintf(out, len, "fat");
      break;
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
      snprintf(out, len, "spiffs");
      break;
    case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
      snprintf(out, len, "littlefs");
      break;
    default:
      snprintf(out, len, "0x%02X", static_cast<unsigned>(p->subtype));
      break;
  }
}

inline uint32_t image_length(const esp_partition_t *p) {
  const esp_partition_pos_t position = {.offset = p->address, .size = p->size};
  esp_image_metadata_t metadata{};
  return esp_image_get_metadata(&position, &metadata) == ESP_OK ? metadata.image_len : 0;
}

inline void build_id(const esp_partition_t *p, char *out, size_t len) {
  esp_app_desc_t desc;
  out[0] = '\0';
  if (esp_ota_get_partition_description(p, &desc) != ESP_OK)
    return;
  size_t at = 0;
  for (size_t i = 0; i < 8 && at + 2 < len; i++)
    at += snprintf(out + at, len - at, "%02x", desc.app_elf_sha256[i]);
}

inline void build_time(const esp_partition_t *p, char *out, size_t len) {
  esp_app_desc_t desc;
  out[0] = '\0';
  if (esp_ota_get_partition_description(p, &desc) == ESP_OK)
    snprintf(out, len, "%s %s", desc.date, desc.time);
}

inline uint64_t used_bytes() {
  uint64_t used = 0;
  const esp_partition_t *first = nullptr;
  for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
       it != nullptr; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    if (first == nullptr || p->address < first->address)
      first = p;
    used += p->type == ESP_PARTITION_TYPE_APP ? image_length(p) : p->size;
  }
  return used + (first != nullptr ? first->address : 0);
}

template<typename F> void for_each_partition(F &&row) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_boot_partition();
  char title[64];
  char detail[112];
  char subtype[16];
  char size[16];
  for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
       it != nullptr; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    subtype_name(p, subtype, sizeof(subtype));
    snprintf(title, sizeof(title), "%s  %s / %s", p->label, type_name(p), subtype);
    size_text(size, sizeof(size), p->size);
    if (p->type == ESP_PARTITION_TYPE_APP) {
      char used[16];
      char id[17];
      const uint32_t length = image_length(p);
      size_text(used, sizeof(used), length);
      build_id(p, id, sizeof(id));
      if (length == 0) {
        snprintf(detail, sizeof(detail), "0x%" PRIX32 "  %s, no image", p->address, size);
      } else {
        snprintf(detail, sizeof(detail), "0x%" PRIX32 "  %s of %s (%u%%)  %s", p->address, used, size,
                 static_cast<unsigned>(static_cast<uint64_t>(length) * 100 / p->size), id);
      }
    } else if (p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) {
      nvs_stats_t stats{};
      if (nvs_get_stats(p->label, &stats) == ESP_OK) {
        snprintf(detail, sizeof(detail), "0x%" PRIX32 "  %s, %u of %u entries used", p->address, size,
                 static_cast<unsigned>(stats.used_entries), static_cast<unsigned>(stats.total_entries));
      } else {
        snprintf(detail, sizeof(detail), "0x%" PRIX32 "  %s", p->address, size);
      }
#ifdef CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    } else if (p->subtype == ESP_PARTITION_SUBTYPE_DATA_COREDUMP) {
      snprintf(detail, sizeof(detail), "0x%" PRIX32 "  %s, %s", p->address, size,
               esp_core_dump_image_check() == ESP_OK ? "holds a crash dump" : "empty");
#endif
    } else {
      snprintf(detail, sizeof(detail), "0x%" PRIX32 "  %s", p->address, size);
    }
    row(static_cast<const char *>(title), static_cast<const char *>(detail), p == running, p == next);
  }
}

}

#endif
