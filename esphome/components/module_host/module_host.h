#pragma once
#ifdef USE_ESP_IDF

#include "esphome/core/component.h"
#include "module_loader.h"
#include "module_abi.h"  // the exact ABI contract the module is built against

namespace esphome {
namespace module_host {

// The stub self-defers through its own loop(); can_proceed() stays true so it never blocks other
// components. A loadable component is non-boot-critical by contract.
enum class ModuleState : uint8_t {
  WAITING_STORAGE,  // the module's filesystem is not mounted yet
  WAITING_MODULE,   // storage up; trying to dlopen + init the module
  ACTIVE,           // module loaded and initialised; loop() forwards into it
  DEGRADED,         // gave up; the feature is unavailable but the device runs
};

class ModuleHost : public Component {
 public:
  void set_module_path(const char *path) { this->module_path_ = path; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

 protected:
  bool storage_ready_();  // is module_path_ reachable (its filesystem mounted)?
  bool try_load_();       // dlopen + resolve required symbols + module_init + L1 proofs

  const char *module_path_{nullptr};
  ModuleState state_{ModuleState::WAITING_STORAGE};
  ModuleLoader loader_;
  LoadedModule module_;
  module_loop_fn loop_fn_{nullptr};
  uint32_t last_attempt_ms_{0};
  uint8_t attempts_{0};
};

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF
