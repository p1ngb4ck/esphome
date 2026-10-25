#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_FLASH_ACTIONS

#include "esphome/components/api/api_server.h"
#include "esphome/components/api/user_services.h"
#include "esphome/core/string_ref.h"

#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <sdkconfig.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace esphome::esp32 {

#ifdef USE_API_USER_DEFINED_ACTION_METADATA
inline constexpr const char *const FLASH_INFO_ACTION_STRINGS[] = {"flash_info", nullptr};
inline constexpr const char *const FLASH_VERIFY_ACTION_STRINGS[] = {"flash_verify", nullptr};
#else
inline constexpr const char *const FLASH_INFO_ACTION_STRINGS[] = {"flash_info"};
inline constexpr const char *const FLASH_VERIFY_ACTION_STRINGS[] = {"flash_verify"};
#endif

class FlashInfoAction : public api::UserServiceBase<> {
 public:
  explicit FlashInfoAction(uint32_t key)
      : api::UserServiceBase<>(FLASH_INFO_ACTION_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {
    if (call_id == 0)
      return;
    uint32_t id = 0;
    uint32_t size = 0;
    if (esp_flash_read_id(esp_flash_default_chip, &id) != ESP_OK ||
        esp_flash_get_physical_size(esp_flash_default_chip, &size) != ESP_OK) {
      api::global_api_server->send_action_response(call_id, false, StringRef("Reading the flash ID failed"));
      api::global_api_server->unregister_active_action_call(call_id);
      return;
    }
    char json[80];
    const int len = snprintf(json, sizeof(json), "{\"id\":\"%06" PRIx32 "\",\"size\":%" PRIu32 ",\"frequency\":\"%uMHZ\"}",
                             id & 0xFFFFFF, size, static_cast<unsigned>(CONFIG_ESPTOOLPY_FLASHFREQ_VAL));
    api::global_api_server->send_action_response(call_id, true, StringRef(""), reinterpret_cast<const uint8_t *>(json),
                                                 static_cast<size_t>(len));
    api::global_api_server->unregister_active_action_call(call_id);
  }
};

class FlashVerifyAction : public api::UserServiceBase<> {
 public:
  explicit FlashVerifyAction(uint32_t key)
      : api::UserServiceBase<>(FLASH_VERIFY_ACTION_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {
    if (call_id == 0)
      return;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_pos_t position = {.offset = running->address, .size = running->size};
    esp_image_metadata_t metadata{};
    const bool ok = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) == ESP_OK;
    char json[96];
    const int len = snprintf(json, sizeof(json), "{\"ok\":%s,\"frequency\":\"%uMHZ\",\"bytes\":%" PRIu32 "}",
                             ok ? "true" : "false", static_cast<unsigned>(CONFIG_ESPTOOLPY_FLASHFREQ_VAL),
                             ok ? metadata.image_len : 0);
    api::global_api_server->send_action_response(call_id, true, StringRef(""), reinterpret_cast<const uint8_t *>(json),
                                                 static_cast<size_t>(len));
    api::global_api_server->unregister_active_action_call(call_id);
  }
};

}

#endif
