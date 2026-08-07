#pragma once
#ifdef USE_ESP_IDF

#include "esp_elf.h"  // esp_elf_t, esp_elf_init/relocate/deinit (pulls the symtab struct in)

namespace esphome {
namespace module_host {

// A module loaded from a buffer via esp_elf_relocate(). The esp_elf_t is heap-allocated and only
// ever referenced through a stable pointer -- exactly like the loader's own dlmod path (elf_dl).
// It must NOT be copied by value: it owns coupled allocations (segment memory, symbol table) and a
// bitwise copy followed by the original going away would leave dangling references. LoadedModule
// therefore holds a pointer, so passing it around copies only the pointer.
class LoadedModule {
 public:
  bool ok() const { return this->loaded_; }
  void *symbol(const char *name) const;

  esp_elf_t *elf_{nullptr};   // heap-allocated, stable for the module's lifetime
  uint8_t *buffer_{nullptr};  // the .so bytes, kept alive while loaded
  bool loaded_{false};
};

class ModuleLoader {
 public:
  LoadedModule load(const char *vfs_path);
  void unload(LoadedModule &mod);
  const char *last_error() const { return this->last_error_; }

 protected:
  const char *last_error_{nullptr};
};

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF
