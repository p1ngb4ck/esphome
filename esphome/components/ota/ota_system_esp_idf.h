#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32
#include "esphome/core/automation.h"
#ifdef USE_API_CUSTOM_SERVICES
#include "esphome/components/api/user_services.h"
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
