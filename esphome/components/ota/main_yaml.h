#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef USE_OTA_MAIN_YAML
#include "esphome/components/store_yaml/store_yaml.h"
#endif

namespace esphome::ota {

static constexpr uint32_t MAIN_YAML_RECORD_MAGIC = 0x4C4D5945;

struct MainYamlRecord {
  uint32_t magic;
  const uint8_t *data;
  uint32_t size;
};

inline void keep_main_yaml_record(const MainYamlRecord *record) { asm volatile("" : : "r"(record)); }

#ifdef USE_OTA_MAIN_YAML
struct MainYaml {
  const uint8_t *data;
  size_t size;
  size_t uncompressed_size;
};

void map_main_yaml();
const MainYaml &main_yaml();
bool export_main_yaml(const std::string &path);
#endif

}

#endif
