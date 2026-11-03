#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_COREDUMP_ACTIONS

#include "esphome/components/api/api_server.h"
#include "esphome/components/api/user_services.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"

#include <esp_core_dump.h>
#include <esp_flash.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace esphome::esp32 {

static constexpr size_t COREDUMP_CHUNK_SIZE = 32 * 1024;

#ifdef USE_API_USER_DEFINED_ACTION_METADATA
inline constexpr const char *const COREDUMP_INFO_STRINGS[] = {"coredump_info", nullptr};
inline constexpr const char *const COREDUMP_ERASE_STRINGS[] = {"coredump_erase", nullptr};
#ifdef USE_API_USER_DEFINED_ACTION_OPTIONAL_ARGS
inline constexpr const char *const COREDUMP_READ_STRINGS[] = {"coredump_read", nullptr, "offset", nullptr, nullptr, nullptr};
#else
inline constexpr const char *const COREDUMP_READ_STRINGS[] = {"coredump_read", nullptr, "offset", nullptr, nullptr};
#endif
#else
inline constexpr const char *const COREDUMP_INFO_STRINGS[] = {"coredump_info"};
inline constexpr const char *const COREDUMP_ERASE_STRINGS[] = {"coredump_erase"};
#ifdef USE_API_USER_DEFINED_ACTION_OPTIONAL_ARGS
inline constexpr const char *const COREDUMP_READ_STRINGS[] = {"coredump_read", "offset", nullptr};
#else
inline constexpr const char *const COREDUMP_READ_STRINGS[] = {"coredump_read", "offset"};
#endif
#endif

inline void coredump_answer(uint32_t call_id, bool success, const char *error, const uint8_t *data, size_t len) {
  api::global_api_server->send_action_response(call_id, success, StringRef(error), data, len);
  api::global_api_server->unregister_active_action_call(call_id);
}

class CoredumpInfoAction : public api::UserServiceBase<> {
 public:
  explicit CoredumpInfoAction(uint32_t key)
      : api::UserServiceBase<>(COREDUMP_INFO_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {
    if (call_id == 0)
      return;
    size_t address = 0;
    size_t size = 0;
    char json[256];
    if (esp_core_dump_image_get(&address, &size) != ESP_OK || size == 0) {
      const int len = snprintf(json, sizeof(json), "{\"present\":false}");
      coredump_answer(call_id, true, "", reinterpret_cast<const uint8_t *>(json), static_cast<size_t>(len));
      return;
    }
    RAMAllocator<esp_core_dump_summary_t> allocator(RAMAllocator<esp_core_dump_summary_t>::ALLOC_EXTERNAL);
    esp_core_dump_summary_t *summary = allocator.allocate(1);
    char reason[64] = "";
    esp_core_dump_get_panic_reason(reason, sizeof(reason));
    for (char *c = reason; *c != '\0'; c++) {
      if (*c == '"' || *c == '\\' || static_cast<unsigned char>(*c) < 0x20)
        *c = ' ';
    }
    int len;
    if (summary != nullptr && esp_core_dump_get_summary(summary) == ESP_OK) {
      summary->exc_task[sizeof(summary->exc_task) - 1] = '\0';
      len = snprintf(json, sizeof(json),
                     "{\"present\":true,\"size\":%u,\"elf_sha256\":\"%.64s\",\"task\":\"%s\",\"pc\":\"0x%08" PRIx32
                     "\",\"reason\":\"%s\"}",
                     static_cast<unsigned>(size), reinterpret_cast<const char *>(summary->app_elf_sha256),
                     summary->exc_task, summary->exc_pc, reason);
    } else {
      len = snprintf(json, sizeof(json), "{\"present\":true,\"size\":%u,\"reason\":\"%s\"}",
                     static_cast<unsigned>(size), reason);
    }
    if (summary != nullptr)
      allocator.deallocate(summary, 1);
    coredump_answer(call_id, true, "", reinterpret_cast<const uint8_t *>(json), static_cast<size_t>(len));
  }
};

class CoredumpReadAction : public api::UserServiceBase<int32_t> {
 public:
  explicit CoredumpReadAction(uint32_t key)
      : api::UserServiceBase<int32_t>(COREDUMP_READ_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response, int32_t offset) override {
    if (call_id == 0)
      return;
    size_t address = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&address, &size) != ESP_OK || offset < 0 || static_cast<size_t>(offset) > size) {
      coredump_answer(call_id, false, "No crash dump at this offset", nullptr, 0);
      return;
    }
    const size_t len = std::min(COREDUMP_CHUNK_SIZE, size - static_cast<size_t>(offset));
    RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    uint8_t *chunk = allocator.allocate(len == 0 ? 1 : len);
    if (chunk == nullptr) {
      coredump_answer(call_id, false, "No PSRAM for the chunk", nullptr, 0);
      return;
    }
    if (len > 0 && esp_flash_read(nullptr, chunk, address + offset, len) != ESP_OK) {
      allocator.deallocate(chunk, len);
      coredump_answer(call_id, false, "Reading the crash dump failed", nullptr, 0);
      return;
    }
    coredump_answer(call_id, true, "", chunk, len);
    allocator.deallocate(chunk, len == 0 ? 1 : len);
  }
};

class CoredumpEraseAction : public api::UserServiceBase<> {
 public:
  explicit CoredumpEraseAction(uint32_t key)
      : api::UserServiceBase<>(COREDUMP_ERASE_STRINGS, key, api::enums::SUPPORTS_RESPONSE_ONLY) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {
    if (call_id == 0)
      return;
    const bool erased = esp_core_dump_image_erase() == ESP_OK;
    static constexpr char ANSWER[] = "{\"erased\":true}";
    if (erased) {
      coredump_answer(call_id, true, "", reinterpret_cast<const uint8_t *>(ANSWER), sizeof(ANSWER) - 1);
    } else {
      coredump_answer(call_id, false, "Erasing the crash dump failed", nullptr, 0);
    }
  }
};

}

#endif
