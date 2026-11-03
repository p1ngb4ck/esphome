#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER) && defined(USE_PACKAGE_MANAGER_API)

#include "package_manager.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/user_services.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/string_ref.h"

#include <array>
#include <string>

namespace esphome::package_manager {

template<size_t N> struct ApiActionStrings {
  static constexpr size_t SIZE = api::USER_ACTION_HEADER_STRINGS + N * api::USER_ACTION_ARG_STRINGS;
  std::array<const char *, SIZE> table{};
  constexpr ApiActionStrings(const char *name, std::array<const char *, N> args) {
    this->table[0] = name;
    for (size_t i = 0; i < N; i++)
      this->table[api::USER_ACTION_HEADER_STRINGS + i * api::USER_ACTION_ARG_STRINGS] = args[i];
  }
};

template<typename... Ts> class PackageStatusAction : public api::UserServiceBase<Ts...> {
 public:
  using Fn = bool (*)(PackageManager &, Ts...);
  PackageStatusAction(const char *const *strings, uint32_t key, PackageManager *pm, Fn fn)
      : api::UserServiceBase<Ts...>(strings, key, api::enums::SUPPORTS_RESPONSE_STATUS), pm_(pm), fn_(fn) {}

 protected:
  void execute(uint32_t call_id, bool return_response, Ts... x) override {
    const bool ok = this->fn_(*this->pm_, x...);
    if (call_id == 0)
      return;
    api::global_api_server->send_action_response(call_id, ok, StringRef(ok ? "" : this->pm_->last_error()));
    api::global_api_server->unregister_active_action_call(call_id);
  }

  PackageManager *pm_;
  Fn fn_;
};

class PackageDataAction : public api::UserServiceBase<> {
 public:
  using Fn = void (*)(PackageManager &, JsonObject);
  PackageDataAction(const char *const *strings, uint32_t key, PackageManager *pm, Fn fn)
      : api::UserServiceBase<>(strings, key, api::enums::SUPPORTS_RESPONSE_ONLY), pm_(pm), fn_(fn) {}

 protected:
  void execute(uint32_t call_id, bool return_response) override {
    if (call_id == 0)
      return;
    json::JsonBuilder builder;
    this->fn_(*this->pm_, builder.root());
    auto buf = builder.serialize();
    api::global_api_server->send_action_response(call_id, true, StringRef(""),
                                                 reinterpret_cast<const uint8_t *>(buf.data()), buf.size());
    api::global_api_server->unregister_active_action_call(call_id);
  }

  PackageManager *pm_;
  Fn fn_;
};

class PackageQueryAction : public api::UserServiceBase<std::string> {
 public:
  using Fn = void (*)(PackageManager &, const std::string &, JsonObject);
  PackageQueryAction(const char *const *strings, uint32_t key, PackageManager *pm, Fn fn)
      : api::UserServiceBase<std::string>(strings, key, api::enums::SUPPORTS_RESPONSE_ONLY), pm_(pm), fn_(fn) {}

 protected:
  void execute(uint32_t call_id, bool return_response, std::string arg) override {
    if (call_id == 0)
      return;
    json::JsonBuilder builder;
    this->fn_(*this->pm_, arg, builder.root());
    auto buf = builder.serialize();
    api::global_api_server->send_action_response(call_id, true, StringRef(""),
                                                 reinterpret_cast<const uint8_t *>(buf.data()), buf.size());
    api::global_api_server->unregister_active_action_call(call_id);
  }

  PackageManager *pm_;
  Fn fn_;
};

}

#endif
