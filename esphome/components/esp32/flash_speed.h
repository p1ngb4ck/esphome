#pragma once

#ifdef USE_ESP32

#include <esp_flash.h>
#include <hal/efuse_hal.h>

#include <cstdint>

namespace esphome::esp32 {

inline bool main_flash_80mhz_capable() {
  uint32_t id = 0;
  if (esp_flash_read_id(esp_flash_default_chip, &id) != ESP_OK)
    return false;
  switch ((id >> 16) & 0xFF) {
    case 0xC8:
    case 0x9D:
    case 0xC2:
    case 0xEF:
    case 0x68:
    case 0xCD:
      break;
    default:
      return false;
  }
  return efuse_hal_chip_revision() >= 1;
}

}

#endif
