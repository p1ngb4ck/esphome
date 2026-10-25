#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32
#include "esphome/core/automation.h"
#ifdef USE_API_CUSTOM_SERVICES
#include "esphome/components/api/user_services.h"
#endif
#ifdef USE_OTA_SYSTEM_ELF_ACTION
#include "esphome/components/api/api_server.h"
#include "esphome/core/string_ref.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <cstdio>
#endif

namespace esphome::ota {

#ifdef USE_OTA_REBOOT_TO_SYSTEM
bool reboot_to_system();

template<typename... Ts> class RebootToSystemAction : public Action<Ts...> {
 public:
  void play(const Ts &...x) override { reboot_to_system(); }
};

#ifdef USE_API_CUSTOM_SERVICES
class RebootToSystemService : public api::UserServiceDynamic<> {
 public:
  RebootToSystemService() : api::UserServiceDynamic<>("reboot_to_ota_system", {}) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override { reboot_to_system(); }
};
#endif
#endif

#ifdef USE_OTA_REBOOT_TO_APP
bool reboot_to_app();

template<typename... Ts> class RebootToAppAction : public Action<Ts...> {
 public:
  void play(const Ts &...x) override { reboot_to_app(); }
};

#ifdef USE_API_CUSTOM_SERVICES
class RebootToAppService : public api::UserServiceDynamic<> {
 public:
  RebootToAppService() : api::UserServiceDynamic<>("reboot_to_main", {}) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override { reboot_to_app(); }
};
#endif
#endif

#ifdef USE_OTA_SYSTEM_ELF_ACTION
#ifdef USE_API_USER_DEFINED_ACTION_METADATA
inline constexpr const char *const OTA_SYSTEM_ELF_ACTION_STRINGS[] = {"ota_system_elf", nullptr};
#else
inline constexpr const char *const OTA_SYSTEM_ELF_ACTION_STRINGS[] = {"ota_system_elf"};
#endif

class OtaSystemElfAction : public api::UserServiceBase<> {
 public:
  explicit OtaSystemElfAction(uint32_t key)
      : api::UserServiceBase<>(OTA_SYSTEM_ELF_ACTION_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {
    if (call_id == 0)
      return;
    const esp_partition_t *system =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    esp_app_desc_t desc;
    if (system == nullptr || esp_ota_get_partition_description(system, &desc) != ESP_OK) {
      api::global_api_server->send_action_response(call_id, false, StringRef("No OTA system image"));
      api::global_api_server->unregister_active_action_call(call_id);
      return;
    }
    char json[200];
    int len = snprintf(json, sizeof(json), "{\"elf_sha256\":\"");
    for (uint8_t byte : desc.app_elf_sha256)
      len += snprintf(json + len, sizeof(json) - len, "%02x", byte);
    const esp_partition_t *app =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    esp_app_desc_t app_desc;
    if (app != nullptr && esp_ota_get_partition_description(app, &app_desc) == ESP_OK) {
      len += snprintf(json + len, sizeof(json) - len, "\",\"main_elf_sha256\":\"");
      for (uint8_t byte : app_desc.app_elf_sha256)
        len += snprintf(json + len, sizeof(json) - len, "%02x", byte);
    }
#ifdef USE_OTA_SYSTEM_SELF_UPDATE
    len += snprintf(json + len, sizeof(json) - len, "\",\"self_update\":true}");
#else
    len += snprintf(json + len, sizeof(json) - len, "\"}");
#endif
    api::global_api_server->send_action_response(call_id, true, StringRef(""), reinterpret_cast<const uint8_t *>(json),
                                                 static_cast<size_t>(len));
    api::global_api_server->unregister_active_action_call(call_id);
  }
};
#endif

#if defined(USE_OTA_SYSTEM_BUILD) && defined(USE_API_CUSTOM_SERVICES)
class SystemRunningService : public api::UserServiceDynamic<> {
 public:
  SystemRunningService() : api::UserServiceDynamic<>("ota_system_running", {}) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {}
};
#endif

}

#endif
