#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_NVS_ACTIONS

#include "nvs_browse.h"

#include "esphome/components/api/api_server.h"
#include "esphome/components/api/user_services.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"

#include <cstdio>
#include <string>

namespace esphome::esp32 {

static constexpr size_t NVS_LIST_PAGE = 20;
static constexpr size_t NVS_LIST_BUFFER = 4096;
static constexpr char NVS_ERASE_EVERYTHING[] = "*";

#if defined(USE_API_USER_DEFINED_ACTION_METADATA) && defined(USE_API_USER_DEFINED_ACTION_OPTIONAL_ARGS)
inline constexpr const char *const NVS_LIST_STRINGS[] = {"nvs_list", nullptr, "namespace", nullptr, nullptr, nullptr,
                                                         "page", nullptr, nullptr, nullptr};
inline constexpr const char *const NVS_ERASE_STRINGS[] = {"nvs_erase", nullptr, "namespace", nullptr, nullptr, nullptr,
                                                          "key", nullptr, nullptr, nullptr};
#elif defined(USE_API_USER_DEFINED_ACTION_METADATA)
inline constexpr const char *const NVS_LIST_STRINGS[] = {"nvs_list", nullptr, "namespace", nullptr, nullptr,
                                                         "page", nullptr, nullptr};
inline constexpr const char *const NVS_ERASE_STRINGS[] = {"nvs_erase", nullptr, "namespace", nullptr, nullptr,
                                                          "key", nullptr, nullptr};
#elif defined(USE_API_USER_DEFINED_ACTION_OPTIONAL_ARGS)
inline constexpr const char *const NVS_LIST_STRINGS[] = {"nvs_list", "namespace", nullptr, "page", nullptr};
inline constexpr const char *const NVS_ERASE_STRINGS[] = {"nvs_erase", "namespace", nullptr, "key", nullptr};
#else
inline constexpr const char *const NVS_LIST_STRINGS[] = {"nvs_list", "namespace", "page"};
inline constexpr const char *const NVS_ERASE_STRINGS[] = {"nvs_erase", "namespace", "key"};
#endif

class NvsListAction : public api::UserServiceBase<std::string, int32_t> {
 public:
  explicit NvsListAction(uint32_t key)
      : api::UserServiceBase<std::string, int32_t>(NVS_LIST_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response, std::string name, int32_t page) override {
    if (call_id == 0)
      return;
    RAMAllocator<char> allocator(RAMAllocator<char>::ALLOC_EXTERNAL);
    char *json = allocator.allocate(NVS_LIST_BUFFER);
    if (json == nullptr) {
      api::global_api_server->send_action_response(call_id, false, StringRef("No PSRAM for the answer"));
      api::global_api_server->unregister_active_action_call(call_id);
      return;
    }
    size_t len = 0;
    auto put = [&](const char *format, auto... args) {
      if (len < NVS_LIST_BUFFER)
        len += snprintf(json + len, NVS_LIST_BUFFER - len, format, args...);
    };
    char usage[48];
    nvs_browse::usage_text(usage, sizeof(usage));
    if (name.empty()) {
      nvs_browse::Namespace found[nvs_browse::MAX_NAMESPACES];
      const size_t count = nvs_browse::namespaces(found, nvs_browse::MAX_NAMESPACES);
      put("{\"usage\":\"%s\",\"namespaces\":[", usage);
      for (size_t i = 0; i < count; i++)
        put("%s{\"name\":\"%s\",\"count\":%u}", i == 0 ? "" : ",", found[i].name, static_cast<unsigned>(found[i].count));
      put("]}");
    } else {
      const size_t skip = page > 0 ? static_cast<size_t>(page) * NVS_LIST_PAGE : 0;
      bool first = true;
      put("{\"namespace\":\"%s\",\"page\":%d,\"entries\":[", name.c_str(), static_cast<int>(page > 0 ? page : 0));
      const size_t total = nvs_browse::entries(name.c_str(), skip, NVS_LIST_PAGE,
                                               [&](const char *key, const char *type, const char *value) {
                                                 put("%s{\"key\":\"%s\",\"type\":\"%s\",\"value\":\"%s\"}",
                                                     first ? "" : ",", key, type, value);
                                                 first = false;
                                               });
      put("],\"total\":%u}", static_cast<unsigned>(total));
    }
    const bool fits = len < NVS_LIST_BUFFER;
    api::global_api_server->send_action_response(call_id, fits, StringRef(fits ? "" : "Answer too long"),
                                                 reinterpret_cast<const uint8_t *>(json), fits ? len : 0);
    api::global_api_server->unregister_active_action_call(call_id);
    allocator.deallocate(json, NVS_LIST_BUFFER);
  }
};

class NvsEraseAction : public api::UserServiceBase<std::string, std::string> {
 public:
  explicit NvsEraseAction(uint32_t key)
      : api::UserServiceBase<std::string, std::string>(NVS_ERASE_STRINGS, key, api::enums::SUPPORTS_RESPONSE_STATUS) {}

 protected:
  void execute(uint32_t call_id, bool return_response, std::string name, std::string key) override {
    if (name == NVS_ERASE_EVERYTHING) {
      const bool erased = nvs_browse::erase_everything();
      if (call_id != 0) {
        api::global_api_server->send_action_response(call_id, erased, StringRef(erased ? "" : "Erasing NVS failed"));
        api::global_api_server->unregister_active_action_call(call_id);
      }
      if (erased)
        App.safe_reboot();
      return;
    }
    const bool erased = !name.empty() && nvs_browse::erase(name.c_str(), key.c_str());
    if (call_id != 0) {
      api::global_api_server->send_action_response(call_id, erased, StringRef(erased ? "" : "Nothing erased"));
      api::global_api_server->unregister_active_action_call(call_id);
    }
  }
};

}

#endif
