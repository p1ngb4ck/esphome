#ifdef USE_ESP_IDF

#include "module_loader.h"
#include "esphome/core/log.h"

// The elf_loader dynamic-shared-object API. Requires CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT=y
// (set from codegen). Only this translation unit sees it.
#include "esp_dlfcn.h"

namespace esphome {
namespace module_host {

static const char *const TAG = "module_host.loader";

LoadedModule ModuleLoader::load(const char *path) {
  LoadedModule mod;
  this->last_error_ = nullptr;
  if (path == nullptr) {
    this->last_error_ = "null module path";
    return mod;
  }
  // RTLD_NOW: bind all symbols now; unresolved host symbols fail here, not at first call.
  mod.handle_ = dlopen(path, RTLD_NOW);
  if (mod.handle_ == nullptr) {
    this->last_error_ = dlerror();  // static buffer inside the loader; used immediately below
    ESP_LOGW(TAG, "dlopen('%s') failed: %s", path, this->last_error_ ? this->last_error_ : "unknown");
  }
  return mod;
}

void ModuleLoader::unload(LoadedModule &mod) {
  if (mod.handle_ != nullptr) {
    dlclose(mod.handle_);
    mod.handle_ = nullptr;
  }
}

void *LoadedModule::symbol(const char *name) const {
  if (this->handle_ == nullptr)
    return nullptr;
  return dlsym(this->handle_, name);
}

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF
