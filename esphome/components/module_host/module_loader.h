#pragma once
#ifdef USE_ESP_IDF

namespace esphome {
namespace module_host {

// A loaded module handle plus symbol resolution. Thin wrapper so nothing outside this file includes
// esp_dlfcn.h. All use is control-plane (main task) only.
class LoadedModule {
 public:
  bool ok() const { return this->handle_ != nullptr; }
  // dlsym by name against the module's symbol table; nullptr if absent. Entry points are exported
  // extern "C" so the plain names resolve.
  void *symbol(const char *name) const;

  void *handle_{nullptr};  // opaque dlopen() handle
};

class ModuleLoader {
 public:
  // dlopen(path, RTLD_NOW). RTLD_NOW resolves every symbol at load, so a missing host export shows
  // up here (with a captured dlerror()) instead of crashing on first call. Returns a LoadedModule
  // whose ok() is false on failure; last_error() then holds the reason.
  LoadedModule load(const char *path);
  void unload(LoadedModule &mod);
  const char *last_error() const { return this->last_error_; }

 protected:
  const char *last_error_{nullptr};
};

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF
