#ifdef USE_ESP_IDF

#include "module_host.h"
#ifdef USE_COMPONENT_AS_LIB
#include "lib_stub.h"
#include <cstring>
#include <cstdio>
#endif
#include "esphome/core/log.h"
#include "esphome/core/hal.h"  // millis()

#include <sys/stat.h>

namespace esphome {
namespace module_host {

static const char *const TAG = "module_host";

#ifdef USE_COMPONENT_AS_LIB
// Derive the .so basename with no directory and no extension: /flash/mcp4461.so -> "mcp4461".
static bool lib_basename_(const char *path, char *out, size_t out_len) {
  if (path == nullptr)
    return false;
  const char *slash = std::strrchr(path, '/');
  const char *b = slash ? slash + 1 : path;
  const char *dot = std::strrchr(b, '.');
  size_t len = dot ? static_cast<size_t>(dot - b) : std::strlen(b);
  if (len == 0 || len >= out_len)
    return false;
  std::memcpy(out, b, len);
  out[len] = '\0';
  return true;
}
#endif
static const uint32_t RETRY_INTERVAL_MS = 2000;
static const uint8_t MAX_ATTEMPTS = 5;

// The host API handed to the module. These are the ONLY host functions the module may call through
// the contract (in addition to the loader's generated libc/IDF symbol table). Keep it minimal.
static void host_log(const char *msg) { ESP_LOGI("module", "%s", msg); }
static uint32_t host_millis() { return millis(); }
static const module_host_api_t HOST_API = {
    ESPHOME_MODULE_ABI_VERSION,
    host_log,
    host_millis,
};

void ModuleHost::setup() {
  // Never block. Just enter the state machine; the work happens in loop().
  this->state_ = ModuleState::WAITING_STORAGE;
}

bool ModuleHost::storage_ready_() {
  // L1: treat the module as reachable once its path stat()s successfully -- that requires the
  // backing filesystem to be mounted AND the file to be present. A production build would instead
  // ask the storage registry whether the mount for module_path_ is up, and handle "mounted but
  // file absent" as a distinct (pre-load-needed) state.
  if (this->module_path_ == nullptr)
    return false;
  struct stat st {};
  return ::stat(this->module_path_, &st) == 0;
}

bool ModuleHost::try_load_() {
  this->module_ = this->loader_.load(this->module_path_);
  if (!this->module_.ok()) {
    const char *e = this->loader_.last_error();
    ESP_LOGE(TAG, "load failed for '%s': %s", this->module_path_, e ? e : "unknown");
    return false;
  }

  auto init = reinterpret_cast<module_init_fn>(this->module_.symbol(MODULE_SYM_INIT));
  auto add = reinterpret_cast<module_add_fn>(this->module_.symbol(MODULE_SYM_ADD));
  auto ctor_ran = reinterpret_cast<module_ctor_ran_fn>(this->module_.symbol(MODULE_SYM_CTOR_RAN));
  this->loop_fn_ = reinterpret_cast<module_loop_fn>(this->module_.symbol(MODULE_SYM_LOOP));  // optional

  if (init == nullptr || add == nullptr) {
#ifdef USE_COMPONENT_AS_LIB
    // Not an L1 module. Try the component_as_lib path: real component(s) built by
    // __lib_construct_<name>(deps) and driven by a firmware-side LibComponentStub.
    if (this->try_lib_construct_())
      return true;
#endif
    ESP_LOGE(TAG, "module '%s' has neither L1 (%s/%s) nor a component_as_lib entry", this->module_path_,
             MODULE_SYM_INIT, MODULE_SYM_ADD);
    this->loader_.unload(this->module_);
    return false;
  }

  // module_init runs the module's own .init_array (the loader does not) and validates the ABI.
  uint32_t v = init(&HOST_API);
  if (v != ESPHOME_MODULE_ABI_VERSION) {
    ESP_LOGE(TAG, "module ABI mismatch (module=%u host=%u)", (unsigned) v, (unsigned) ESPHOME_MODULE_ABI_VERSION);
    this->loader_.unload(this->module_);
    return false;
  }

  // L1 proofs: compute path executes from PSRAM, and the init_array probe result is recorded.
  ESP_LOGI(TAG, "module_add(2, 3) = %d (expect 5)", add(2, 3));
  if (ctor_ran != nullptr)
    ESP_LOGI(TAG, "loader ran module init_array before entry: %s (expected: no)",
             ctor_ran() ? "YES" : "no");
  return true;
}

void ModuleHost::loop() {
  switch (this->state_) {
    case ModuleState::WAITING_STORAGE:
      if (this->storage_ready_())
        this->state_ = ModuleState::WAITING_MODULE;
      break;

    case ModuleState::WAITING_MODULE: {
      const uint32_t now = millis();
      if (this->last_attempt_ms_ != 0 && (now - this->last_attempt_ms_) < RETRY_INTERVAL_MS)
        break;
      this->last_attempt_ms_ = now;
      if (this->try_load_()) {
        this->state_ = ModuleState::ACTIVE;
        ESP_LOGI(TAG, "module ACTIVE (%s)", this->module_path_);
      } else if (++this->attempts_ >= MAX_ATTEMPTS) {
        this->state_ = ModuleState::DEGRADED;
        ESP_LOGE(TAG, "giving up after %u attempts; module feature unavailable", this->attempts_);
      }
      break;
    }

    case ModuleState::ACTIVE:
      if (this->loop_fn_ != nullptr)
        this->loop_fn_();
      break;

    case ModuleState::DEGRADED:
      break;
  }
}

float ModuleHost::get_setup_priority() const { return setup_priority::LATE; }

void ModuleHost::dump_config() {
  const char *s = "?";
  switch (this->state_) {
    case ModuleState::WAITING_STORAGE:
      s = "waiting for storage";
      break;
    case ModuleState::WAITING_MODULE:
      s = "waiting for module";
      break;
    case ModuleState::ACTIVE:
      s = "active";
      break;
    case ModuleState::DEGRADED:
      s = "degraded (module not loaded)";
      break;
  }
  ESP_LOGCONFIG(TAG, "Module host:");
  ESP_LOGCONFIG(TAG, "  path: %s", this->module_path_ != nullptr ? this->module_path_ : "(none)");
  ESP_LOGCONFIG(TAG, "  state: %s", s);
}

#ifdef USE_COMPONENT_AS_LIB
bool ModuleHost::try_lib_construct_() {
  char base[64];
  if (!lib_basename_(this->module_path_, base, sizeof(base)))
    return false;
  const LibEntry *entry = find_lib(base);
  if (entry == nullptr)
    return false;  // not a component_as_lib .so we were told about
  char sym[96];
  std::snprintf(sym, sizeof(sym), "__lib_construct_%s", base);
  using construct_fn = Component **(*) (void **, uint32_t *);
  auto construct = reinterpret_cast<construct_fn>(this->module_.symbol(sym));
  if (construct == nullptr) {
    ESP_LOGE(TAG, "lib '%s': entry symbol %s not found in module", base, sym);
    return false;
  }
  uint32_t count = 0;
  Component **reals = construct(entry->deps, &count);
  entry->stub->attach(reals, count);
  ESP_LOGI(TAG, "lib '%s': constructed %u component(s), attached to stub", base, (unsigned) count);
  return true;
}
#endif

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF
