#include "main_yaml.h"

#if defined(USE_ESP32) && defined(USE_OTA_MAIN_YAML)
#include "esphome/core/log.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_partition.h>

#include <cinttypes>
#include <cstring>

extern "C" unsigned long long ZSTD_getFrameContentSize(const void *src, size_t src_size);

namespace esphome::ota {

ESPHOME_LOG_TAG(TAG, "ota.system");

static constexpr size_t MAIN_YAML_RECORD_OFFSET =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
static constexpr uint8_t ZSTD_FRAME_MAGIC[] = {0x28, 0xB5, 0x2F, 0xFD};
static constexpr unsigned long long ZSTD_CONTENT_SIZE_ERROR = 0ULL - 2;

static bool find_main_yaml(const esp_partition_t *app, uint32_t *offset, uint32_t *size) {
  MainYamlRecord record{};
  if (esp_partition_read(app, MAIN_YAML_RECORD_OFFSET, &record, sizeof(record)) != ESP_OK ||
      record.magic != MAIN_YAML_RECORD_MAGIC || record.size == 0 || record.size > app->size)
    return false;
  esp_image_header_t header{};
  if (esp_partition_read(app, 0, &header, sizeof(header)) != ESP_OK || header.magic != ESP_IMAGE_HEADER_MAGIC)
    return false;
  const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(record.data));
  uint32_t position = sizeof(esp_image_header_t);
  for (uint8_t i = 0; i < header.segment_count; i++) {
    esp_image_segment_header_t segment{};
    if (position + sizeof(segment) > app->size || esp_partition_read(app, position, &segment, sizeof(segment)) != ESP_OK)
      return false;
    position += sizeof(segment);
    if (address >= segment.load_addr && address - segment.load_addr <= segment.data_len &&
        record.size <= segment.data_len - (address - segment.load_addr)) {
      *offset = position + (address - segment.load_addr);
      *size = record.size;
      return *offset + *size <= app->size;
    }
    position += segment.data_len;
  }
  return false;
}

static MainYaml found_main_yaml{};

void map_main_yaml() {
  const esp_partition_t *app =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  uint32_t offset = 0;
  uint32_t size = 0;
  if (app == nullptr || !find_main_yaml(app, &offset, &size)) {
    ESP_LOGD(TAG, "The main firmware embeds no YAML");
    return;
  }
  const void *data = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(app, offset, size, ESP_PARTITION_MMAP_DATA, &data, &handle) != ESP_OK)
    return;
  if (memcmp(data, ZSTD_FRAME_MAGIC, sizeof(ZSTD_FRAME_MAGIC)) != 0) {
    esp_partition_munmap(handle);
    ESP_LOGD(TAG, "The main firmware's YAML record points to no zstd frame");
    return;
  }
  const unsigned long long content = ZSTD_getFrameContentSize(data, size);
  found_main_yaml.data = static_cast<const uint8_t *>(data);
  found_main_yaml.size = size;
  found_main_yaml.uncompressed_size = content < ZSTD_CONTENT_SIZE_ERROR ? static_cast<size_t>(content) : 0;
  ESP_LOGD(TAG, "Main firmware YAML: %" PRIu32 " bytes (%zu uncompressed)", size, found_main_yaml.uncompressed_size);
}

const MainYaml &main_yaml() { return found_main_yaml; }

bool export_main_yaml(const std::string &path) {
  if (store_yaml::global_store_yaml == nullptr)
    return false;
  return store_yaml::global_store_yaml->export_blob_to_storage(found_main_yaml.data, found_main_yaml.size,
                                                               found_main_yaml.uncompressed_size, path.c_str(), false);
}

}

#endif
