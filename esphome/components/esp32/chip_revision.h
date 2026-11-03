#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_CHIP_REVISION_ACTION

#include "esphome/components/api/api_server.h"
#include "esphome/components/api/user_services.h"
#include "esphome/core/string_ref.h"

#include <esp_chip_info.h>

#include <cstdint>
#include <cstdio>

namespace esphome::esp32 {

#ifdef USE_API_USER_DEFINED_ACTION_METADATA
inline constexpr const char *const CHIP_REVISION_ACTION_STRINGS[] = {"chip_revision", nullptr};
#else
inline constexpr const char *const CHIP_REVISION_ACTION_STRINGS[] = {"chip_revision"};
#endif

class ChipRevisionAction : public api::UserServiceBase<> {
 public:
  explicit ChipRevisionAction(uint32_t key)
      : api::UserServiceBase<>(CHIP_REVISION_ACTION_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {
    if (call_id == 0)
      return;
    esp_chip_info_t info;
    esp_chip_info(&info);
    char json[24];
    const int len = snprintf(json, sizeof(json), "{\"revision\":%u}", static_cast<unsigned>(info.revision));
    api::global_api_server->send_action_response(call_id, true, StringRef(""), reinterpret_cast<const uint8_t *>(json),
                                                 static_cast<size_t>(len));
    api::global_api_server->unregister_active_action_call(call_id);
  }
};

}

#endif
